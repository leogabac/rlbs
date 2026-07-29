#include <rlbs/control/unix_socket.hpp>

#include <cerrno>
#include <cstring>
#include <string_view>
#include <utility>
#include <vector>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <rlbs/control/protocol.hpp>
#include <rlbs/local/coordinator.hpp>

namespace rlbs {
namespace {

// quick syscall glossary for the rest of this file:
// - socket() asks the kernel for a communication endpoint and returns an fd
// - bind() gives the server fd a filesystem name clients can find
// - listen() turns that fd into a queue of incoming connections
// - accept() removes one connection from that queue and returns a client fd
// - connect() attaches a client fd to the named server
// - send()/recv() move one framed packet between the connected fds
// - unlink() removes the filesystem name; it does not close an fd by itself

// same single-owner deal as uniquefd in the process runner, just named for what
// this fd does here. the destructor catches every unhappy return path
class UniqueSocket {
  public:
    explicit UniqueSocket(int socket = -1) : socket_{socket} {}

    UniqueSocket(const UniqueSocket&) = delete;
    UniqueSocket& operator=(const UniqueSocket&) = delete;

    UniqueSocket(UniqueSocket&& other) noexcept
        : socket_{std::exchange(other.socket_, -1)} {}

    UniqueSocket& operator=(UniqueSocket&&) = delete;

    ~UniqueSocket() {
        if (socket_ >= 0) {
            static_cast<void>(::close(socket_));
        }
    }

    [[nodiscard]] int get() const { return socket_; }

    [[nodiscard]] int release() { return std::exchange(socket_, -1); }

  private:
    int socket_{-1};
};

[[nodiscard]] ControlSocketError error(ControlSocketOperation operation,
                                       int system_error, std::string message) {
    return {
        .operation = operation,
        .system_error = system_error,
        .message = std::move(message),
    };
}

[[nodiscard]] std::expected<sockaddr_un, ControlSocketError>
socket_address(const std::filesystem::path& path) {
    const auto native = path.string();
    sockaddr_un address{};
    address.sun_family = AF_UNIX;

    // sockaddr_un has a fixed-size ancient char array for its path. check
    // before memcpy so a long config value does not walk over the struct
    if (native.empty() || native.size() >= sizeof(address.sun_path)) {
        return std::unexpected{
            error(ControlSocketOperation::inspect_path, ENAMETOOLONG,
                  "control socket path is empty or too long")};
    }

    std::memcpy(address.sun_path, native.c_str(), native.size() + 1);
    return address;
}

[[nodiscard]] std::expected<void, ControlSocketError>
remove_stale_socket(const std::filesystem::path& path,
                    const sockaddr_un& address) {
    struct stat status{};

    // unix sockets have a real filesystem entry. lstat tells us whether the
    // name is free, a socket from an old crash, or somebody's unrelated file
    if (::lstat(path.c_str(), &status) < 0) {
        if (errno == ENOENT) {
            return {};
        }

        return std::unexpected{
            error(ControlSocketOperation::inspect_path, errno, path.string())};
    }

    if (!S_ISSOCK(status.st_mode)) {
        return std::unexpected{
            error(ControlSocketOperation::inspect_path, EEXIST,
                  "control socket path already exists and is not a socket")};
    }

    // never blindly unlink an existing socket. that would make a live daemon
    // unreachable while its already-open fd kept running as if nothing happened
    UniqueSocket probe{::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0)};

    if (probe.get() < 0) {
        return std::unexpected{
            error(ControlSocketOperation::create_socket, errno, path.string())};
    }

    // a successful probe means another server is alive. connection refused
    // means only the dead filesystem name survived and cleanup is safe
    if (::connect(probe.get(), reinterpret_cast<const sockaddr*>(&address),
                  sizeof(address)) == 0) {
        return std::unexpected{
            error(ControlSocketOperation::bind_socket, EADDRINUSE,
                  "another rlbsd is already using the control socket")};
    }

    if (errno != ECONNREFUSED && errno != ENOENT) {
        return std::unexpected{
            error(ControlSocketOperation::inspect_path, errno, path.string())};
    }

    // a crashed daemon can leave the filesystem name behind even though no
    // socket is listening anymore. only that dead case gets unlinked here
    if (::unlink(path.c_str()) < 0 && errno != ENOENT) {
        return std::unexpected{
            error(ControlSocketOperation::inspect_path, errno, path.string())};
    }

    return {};
}

void set_client_timeouts(int socket) {
    constexpr timeval timeout{
        .tv_sec = 1,
        .tv_usec = 0,
    };
    // accepted clients are blocking for a deliberately simple one-request
    // exchange. these timeouts keep a client that sends nothing from freezing
    // the scheduler forever; long-lived clients can get a state machine later
    static_cast<void>(::setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                                   sizeof(timeout)));
    static_cast<void>(::setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                                   sizeof(timeout)));
}

[[nodiscard]] std::expected<void, ControlSocketError>
send_frame(int socket, const std::vector<std::byte>& frame) {
    // seqpacket sends one whole message or fails. msg_nosignal prevents a
    // disappearing client from delivering sigpipe to the entire daemon
    const auto sent = ::send(socket, frame.data(), frame.size(), MSG_NOSIGNAL);

    if (sent < 0 || static_cast<std::size_t>(sent) != frame.size()) {
        return std::unexpected{error(ControlSocketOperation::send_frame,
                                     sent < 0 ? errno : EIO,
                                     "could not send complete frame")};
    }

    return {};
}

[[nodiscard]] std::expected<std::vector<std::byte>, ControlSocketError>
receive_frame(int socket) {
    // ask for one byte beyond the limit so an oversized packet is
    // distinguishable from a legal packet that exactly fills the buffer
    std::vector<std::byte> frame(max_control_frame_size + 1);
    const auto received = ::recv(socket, frame.data(), frame.size(), 0);

    if (received < 0) {
        return std::unexpected{error(ControlSocketOperation::receive_frame,
                                     errno, "could not receive control frame")};
    }
    if (received == 0) {
        return std::unexpected{
            error(ControlSocketOperation::receive_frame, ECONNRESET,
                  "control connection closed without a frame")};
    }
    if (static_cast<std::size_t>(received) > max_control_frame_size) {
        return std::unexpected{error(ControlSocketOperation::receive_frame,
                                     EMSGSIZE,
                                     "control frame exceeds the size limit")};
    }

    frame.resize(static_cast<std::size_t>(received));
    return frame;
}

[[nodiscard]] ControlResponse handle_request(const ControlRequest& request,
                                             JobRepository& repository,
                                             LocalCoordinator& coordinator) {
    // transport ends here. every command uses repository methods instead of
    // teaching socket code enough sqlite to become a second database layer
    if (const auto* submitted = std::get_if<SubmitRequest>(&request)) {
        auto job = repository.submit(submitted->spec);

        if (!job) {
            return ErrorResponse{.message = job.error().message};
        }

        return SubmitResponse{.job_id = job->id};
    }

    if (std::holds_alternative<QueueRequest>(request)) {
        auto jobs = repository.all();

        if (!jobs) {
            return ErrorResponse{.message = jobs.error().message};
        }

        std::vector<JobSummary> summaries;
        summaries.reserve(jobs->size());

        for (const auto& job : *jobs) {
            summaries.push_back({
                .id = job.id,
                .name = job.spec.name,
                .state = job.state,
                .resources = job.spec.resources,
                .assigned_node = job.assigned_node,
            });
        }

        return QueueResponse{.jobs = std::move(summaries)};
    }

    if (const auto* status = std::get_if<StatusRequest>(&request)) {
        auto job = repository.find(status->job_id);

        if (!job) {
            return ErrorResponse{.message = job.error().message};
        }
        if (!*job) {
            return ErrorResponse{.message = "job " +
                                            std::to_string(status->job_id) +
                                            " was not found"};
        }

        return StatusResponse{.job = std::move(**job)};
    }

    const auto job_id = std::get<CancelRequest>(request).job_id;
    auto cancelled = coordinator.cancel(job_id);

    if (!cancelled) {
        return ErrorResponse{.message = cancelled.error().message};
    }

    return CancelResponse{.job_id = job_id};
}

void send_error_response(int socket, std::string_view message) {
    auto frame =
        encode_response(ErrorResponse{.message = std::string{message}});

    if (frame) {
        static_cast<void>(send_frame(socket, *frame));
    }
}

} // namespace

ControlServer::ControlServer(int socket, std::filesystem::path path,
                             JobRepository& repository,
                             LocalCoordinator& coordinator)
    : socket_{socket}, path_{std::move(path)}, repository_{&repository},
      coordinator_{&coordinator}, owns_path_{true} {}

ControlServer::ControlServer(ControlServer&& other) noexcept
    : socket_{std::exchange(other.socket_, -1)}, path_{std::move(other.path_)},
      repository_{other.repository_}, coordinator_{other.coordinator_},
      owns_path_{std::exchange(other.owns_path_, false)} {}

ControlServer::~ControlServer() { close(); }

std::expected<ControlServer, ControlSocketError>
ControlServer::listen(const std::filesystem::path& path,
                      JobRepository& repository,
                      LocalCoordinator& coordinator) {
    // order matters: validate the address, deal with a stale name, create the
    // fd, bind the name, then listen. later failures undo the filesystem entry
    // so the next startup is not punished for this one
    auto address = socket_address(path);

    if (!address) {
        return std::unexpected{std::move(address.error())};
    }

    if (auto removed = remove_stale_socket(path, *address); !removed) {
        return std::unexpected{std::move(removed.error())};
    }

    // af_unix stays on this machine. seqpacket preserves one request per
    // packet. cloexec keeps job children from inheriting the control fd, while
    // nonblock makes accept return eagain instead of waiting when nobody is
    // queued
    UniqueSocket socket{
        ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)};

    if (socket.get() < 0) {
        return std::unexpected{
            error(ControlSocketOperation::create_socket, errno, path.string())};
    }

    // bind creates the socket file and associates its name with this fd
    if (::bind(socket.get(), reinterpret_cast<const sockaddr*>(&*address),
               sizeof(*address)) < 0) {
        return std::unexpected{
            error(ControlSocketOperation::bind_socket, errno, path.string())};
    }

    // owner and group may submit; everyone else stays out. actual auth can grow
    // later without starting from a world-writable scheduler socket
    if (::chmod(path.c_str(), 0660) < 0) {
        const int permission_error = errno;
        static_cast<void>(::unlink(path.c_str()));
        return std::unexpected{
            error(ControlSocketOperation::bind_socket, permission_error,
                  "could not set control socket permissions")};
    }

    // listen turns the bound fd into a server backlog. 64 is enough for a local
    // cli burst and, more importantly, still finite
    if (::listen(socket.get(), 64) < 0) {
        const int listen_error = errno;
        static_cast<void>(::unlink(path.c_str()));
        return std::unexpected{error(ControlSocketOperation::listen_socket,
                                     listen_error, path.string())};
    }

    return ControlServer{socket.release(), path, repository, coordinator};
}

std::expected<std::size_t, ControlSocketError> ControlServer::poll() {
    std::size_t handled = 0;

    for (;;) {
        // accept4 creates a separate fd for one client. the listening fd stays
        // open for everyone else, and cloexec keeps this fd out of child jobs
        const int client = ::accept4(socket_, nullptr, nullptr, SOCK_CLOEXEC);

        if (client < 0) {
            // eagain is the normal "backlog is empty" result from our
            // nonblocking listener, not an error worth scaring rlbsd with
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return handled;
            }
            if (errno == EINTR) {
                continue;
            }

            return std::unexpected{error(ControlSocketOperation::accept_client,
                                         errno, path_.string())};
        }

        UniqueSocket owned_client{client};
        set_client_timeouts(client);
        handle_client(client);
        ++handled;
    }
}

void ControlServer::handle_client(int client_socket) {
    // one connection carries exactly one request and one response for now. that
    // keeps ownership painfully clear while the command set is still tiny
    auto frame = receive_frame(client_socket);

    if (!frame) {
        send_error_response(client_socket, frame.error().message);
        return;
    }

    auto request = decode_request(*frame);

    if (!request) {
        send_error_response(client_socket, request.error().message);
        return;
    }

    auto response =
        encode_response(handle_request(*request, *repository_, *coordinator_));

    if (!response) {
        send_error_response(client_socket, response.error().message);
        return;
    }

    // a client disappearing should not take the scheduler down with it, so
    // connection-local send failures end here with the connection
    static_cast<void>(send_frame(client_socket, *response));
}

void ControlServer::close() {
    // close releases the kernel endpoint; unlink releases the name clients use.
    // doing only one leaves a different flavor of broken socket behind
    if (socket_ >= 0) {
        static_cast<void>(::close(socket_));
        socket_ = -1;
    }

    if (owns_path_) {
        static_cast<void>(::unlink(path_.c_str()));
        owns_path_ = false;
    }
}

const std::filesystem::path& ControlServer::path() const { return path_; }

ControlClient::ControlClient(std::filesystem::path path)
    : path_{std::move(path)} {}

std::expected<JobId, ControlSocketError>
ControlClient::submit(const JobSpec& spec) const {
    auto response = request(ControlRequest{SubmitRequest{.spec = spec}});

    if (!response) {
        return std::unexpected{std::move(response.error())};
    }
    if (const auto* submitted = std::get_if<SubmitResponse>(&*response)) {
        return submitted->job_id;
    }

    return std::unexpected{
        error(ControlSocketOperation::server_response, EPROTO,
              "daemon returned the wrong response to submit")};
}

std::expected<std::vector<JobSummary>, ControlSocketError>
ControlClient::queue() const {
    auto response = request(ControlRequest{QueueRequest{}});

    if (!response) {
        return std::unexpected{std::move(response.error())};
    }
    if (auto* queue = std::get_if<QueueResponse>(&*response)) {
        return std::move(queue->jobs);
    }

    return std::unexpected{
        error(ControlSocketOperation::server_response, EPROTO,
              "daemon returned the wrong response to queue")};
}

std::expected<Job, ControlSocketError>
ControlClient::status(JobId job_id) const {
    auto response = request(ControlRequest{StatusRequest{.job_id = job_id}});

    if (!response) {
        return std::unexpected{std::move(response.error())};
    }
    if (auto* status = std::get_if<StatusResponse>(&*response)) {
        return std::move(status->job);
    }

    return std::unexpected{
        error(ControlSocketOperation::server_response, EPROTO,
              "daemon returned the wrong response to status")};
}

std::expected<JobId, ControlSocketError>
ControlClient::cancel(JobId job_id) const {
    auto response = request(ControlRequest{CancelRequest{.job_id = job_id}});

    if (!response) {
        return std::unexpected{std::move(response.error())};
    }
    if (const auto* cancelled = std::get_if<CancelResponse>(&*response)) {
        return cancelled->job_id;
    }

    return std::unexpected{
        error(ControlSocketOperation::server_response, EPROTO,
              "daemon returned the wrong response to cancel")};
}

std::expected<ControlResponse, ControlSocketError>
ControlClient::request(const ControlRequest& control_request) const {
    // encode before opening anything so a size error does not create a
    // pointless connection the server then has to clean up
    auto request_frame = encode_request(control_request);

    if (!request_frame) {
        return std::unexpected{error(ControlSocketOperation::encode_frame,
                                     EINVAL, request_frame.error().message)};
    }

    auto address = socket_address(path_);

    if (!address) {
        return std::unexpected{std::move(address.error())};
    }

    // the client is blocking because it sends one small request and wants one
    // answer. unlike the daemon listener, there is no event loop to protect
    UniqueSocket socket{::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0)};

    if (socket.get() < 0) {
        return std::unexpected{error(ControlSocketOperation::create_socket,
                                     errno, path_.string())};
    }

    // connect uses the filesystem path to find the listening rlbsd socket
    if (::connect(socket.get(), reinterpret_cast<const sockaddr*>(&*address),
                  sizeof(*address)) < 0) {
        return std::unexpected{error(ControlSocketOperation::connect_server,
                                     errno, path_.string())};
    }

    if (auto sent = send_frame(socket.get(), *request_frame); !sent) {
        return std::unexpected{std::move(sent.error())};
    }

    auto response_frame = receive_frame(socket.get());

    if (!response_frame) {
        return std::unexpected{std::move(response_frame.error())};
    }

    auto response = decode_response(*response_frame);

    if (!response) {
        return std::unexpected{error(ControlSocketOperation::decode_frame,
                                     EINVAL, response.error().message)};
    }

    if (const auto* failed = std::get_if<ErrorResponse>(&*response)) {
        return std::unexpected{
            error(ControlSocketOperation::server_response, 0, failed->message)};
    }

    return std::move(*response);
}

} // namespace rlbs
