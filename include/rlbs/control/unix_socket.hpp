// the unix socket is the trust and ownership boundary for every local command.
// clients ask; the daemon owns repositories and decides. keeping that direction
// obvious matters even more once user credentials and queue acls arrive.
#pragma once

#include <cstddef>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>

#include <rlbs/control/authorization.hpp>
#include <rlbs/control/protocol.hpp>
#include <rlbs/logging/logger.hpp>
#include <rlbs/persistence/job_repository.hpp>
#include <rlbs/persistence/queue_repository.hpp>

namespace rlbs {

class LocalCoordinator;

enum class ControlSocketOperation {
    inspect_path,
    inspect_peer,
    create_socket,
    bind_socket,
    configure_socket,
    listen_socket,
    accept_client,
    connect_server,
    send_frame,
    receive_frame,
    encode_frame,
    decode_frame,
    handle_request,
    server_response,
};

struct ControlSocketError {
    ControlSocketOperation operation{ControlSocketOperation::create_socket};
    int system_error{0};
    std::string message;
};

// the server owns both a kernel socket fd and the little filesystem entry used
// to find it. moving transfers both responsibilities; copying would eventually
// make two destructors close and unlink the same things, which would be bad
class ControlServer {
  public:
    [[nodiscard]] static std::expected<ControlServer, ControlSocketError>
    listen(const std::filesystem::path& path, JobRepository& repository,
           QueueRepository& queues, LocalCoordinator& coordinator,
           std::optional<std::uint32_t> socket_group_id = std::nullopt,
           Logger* logger = nullptr);

    ControlServer(const ControlServer&) = delete;
    ControlServer& operator=(const ControlServer&) = delete;
    ControlServer(ControlServer&& other) noexcept;
    ControlServer& operator=(ControlServer&&) = delete;
    ~ControlServer();

    // process every client already waiting in the local socket backlog. this
    // does not wait for future clients, so rlbsd can service requests and
    // schedule jobs from the same boring loop
    [[nodiscard]] std::expected<std::size_t, ControlSocketError> poll();
    void close();

    [[nodiscard]] const std::filesystem::path& path() const;

  private:
    ControlServer(int socket, std::filesystem::path path,
                  JobRepository& repository, QueueRepository& queues,
                  LocalCoordinator& coordinator,
                  AuthorizationPolicy authorization, Logger* logger);

    void handle_client(int client_socket);

    int socket_{-1};
    std::filesystem::path path_;
    JobRepository* repository_{nullptr};
    QueueRepository* queues_{nullptr};
    LocalCoordinator* coordinator_{nullptr};
    AuthorizationPolicy authorization_;
    Logger* logger_{nullptr};
    bool owns_path_{false};
};

class ControlClient {
  public:
    explicit ControlClient(std::filesystem::path path);

    // each request gets one short-lived connection for now. persistent
    // connections can wait until there is a real reason to manage them
    [[nodiscard]] std::expected<JobId, ControlSocketError>
    submit(const JobSpec& spec) const;

    [[nodiscard]] std::expected<std::vector<JobSummary>, ControlSocketError>
    queue(bool include_finished = false) const;

    [[nodiscard]] std::expected<Job, ControlSocketError>
    status(JobId job_id) const;

    [[nodiscard]] std::expected<JobId, ControlSocketError>
    cancel(JobId job_id) const;

    [[nodiscard]] std::expected<std::vector<NodeSummary>, ControlSocketError>
    nodes() const;

    [[nodiscard]] std::expected<std::vector<BatchQueue>, ControlSocketError>
    queues() const;

    [[nodiscard]] std::expected<BatchQueue, ControlSocketError>
    add_queue(const BatchQueue& queue) const;

    [[nodiscard]] std::expected<BatchQueue, ControlSocketError>
    update_queue(std::string name, QueueAction action) const;

  private:
    [[nodiscard]] std::expected<ControlResponse, ControlSocketError>
    request(const ControlRequest& request) const;

    std::filesystem::path path_;
};

} // namespace rlbs
