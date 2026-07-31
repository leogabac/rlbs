// jobs write into a daemon-controlled spool, but final paths belong to users.
// the publisher child is the annoying little bridge that keeps both facts true
// without letting a root daemon bypass destination permissions.
#include <rlbs/local/output_spool.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <fstream>
#include <system_error>
#include <utility>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#include <rlbs/execution/identity.hpp>

namespace rlbs {
namespace {

[[nodiscard]] OutputSpoolError error(OutputSpoolOperation operation,
                                     std::string message,
                                     int system_error = 0) {
    return {
        .operation = operation,
        .system_error = system_error,
        .message = std::move(message),
    };
}

// this file owns a few short-lived fds while publishing. one tiny owner class
// is less exciting than auditing every early return for a forgotten close.
class OwnedFd {
  public:
    explicit OwnedFd(int descriptor = -1) : descriptor_{descriptor} {}

    OwnedFd(const OwnedFd&) = delete;
    OwnedFd& operator=(const OwnedFd&) = delete;

    OwnedFd(OwnedFd&& other) noexcept
        : descriptor_{std::exchange(other.descriptor_, -1)} {}

    ~OwnedFd() {
        if (descriptor_ >= 0) {
            static_cast<void>(::close(descriptor_));
        }
    }

    [[nodiscard]] int get() const { return descriptor_; }
    void close() {
        if (descriptor_ >= 0) {
            static_cast<void>(::close(descriptor_));
            descriptor_ = -1;
        }
    }

  private:
    int descriptor_{-1};
};

enum class PublisherChildOperation : int {
    set_supplementary_groups,
    set_group_id,
    set_user_id,
    create_stage_file,
    read_existing_output,
    read_spool,
    write_stage_file,
    finish_stage_file,
    publish_output,
};

// the publisher child cannot send a std::string safely through a pipe. this
// fixed record is enough for the parent to rebuild a useful normal c++ error.
struct PublisherChildFailure {
    PublisherChildOperation operation;
    int system_error;
};

[[noreturn]] void report_publisher_failure(
    int error_pipe, PublisherChildOperation operation, int system_error,
    const char* temporary_path = nullptr) {
    // only the child knows the randomized mkstemp name. clean it here while it
    // still has the user's directory permissions instead of leaking dotfiles.
    if (temporary_path != nullptr) {
        static_cast<void>(::unlink(temporary_path));
    }

    const PublisherChildFailure failure{
        .operation = operation,
        .system_error = system_error,
    };
    const auto* bytes = reinterpret_cast<const char*>(&failure);
    std::size_t written = 0;

    while (written < sizeof(failure)) {
        const auto result =
            ::write(error_pipe, bytes + written, sizeof(failure) - written);

        if (result > 0) {
            written += static_cast<std::size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }

        break;
    }

    ::_exit(127);
}

void copy_fd(int source, int destination, int error_pipe,
             PublisherChildOperation read_operation,
             const char* temporary_path) {
    std::array<char, 64 * 1024> buffer{};

    for (;;) {
        const auto received = ::read(source, buffer.data(), buffer.size());

        if (received == 0) {
            return;
        }
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }

            report_publisher_failure(error_pipe, read_operation, errno,
                                     temporary_path);
        }

        std::size_t written = 0;
        const auto byte_count = static_cast<std::size_t>(received);

        // write is allowed to accept fewer bytes than requested. keep going
        // until this whole read buffer reached the temporary file.
        while (written < byte_count) {
            const auto result =
                ::write(destination, buffer.data() + written,
                        byte_count - written);

            if (result > 0) {
                written += static_cast<std::size_t>(result);
                continue;
            }
            if (result < 0 && errno == EINTR) {
                continue;
            }

            report_publisher_failure(
                error_pipe, PublisherChildOperation::write_stage_file,
                result < 0 ? errno : EIO, temporary_path);
        }
    }
}

[[noreturn]] void publish_child(
    int error_pipe, int spool_file, const std::filesystem::path& destination,
    char* temporary_pattern, bool append_output,
    const PreparedIdentity& identity) {
    // from here on every destination-side syscall runs with the job owner's
    // credentials. root never gets to "help" a forbidden path succeed.
    if (const auto switched = switch_identity(identity)) {
        switch (switched->operation) {
        case IdentityOperation::set_supplementary_groups:
            report_publisher_failure(
                error_pipe,
                PublisherChildOperation::set_supplementary_groups,
                switched->system_error);
        case IdentityOperation::set_group_id:
            report_publisher_failure(error_pipe,
                                     PublisherChildOperation::set_group_id,
                                     switched->system_error);
        case IdentityOperation::set_user_id:
            report_publisher_failure(error_pipe,
                                     PublisherChildOperation::set_user_id,
                                     switched->system_error);
        case IdentityOperation::resolve_account:
        case IdentityOperation::resolve_groups:
            report_publisher_failure(error_pipe,
                                     PublisherChildOperation::set_user_id,
                                     switched->system_error);
        }
    }

    // mkstemp creates the file as the current user with mode 0600. that is a
    // deliberately conservative default until rlbs records submission umasks.
    const int stage_file = ::mkstemp(temporary_pattern);

    if (stage_file < 0) {
        report_publisher_failure(
            error_pipe, PublisherChildOperation::create_stage_file, errno);
    }

    const char* temporary_path = temporary_pattern;

    if (append_output) {
        const int existing = ::open(destination.c_str(), O_RDONLY | O_CLOEXEC);

        if (existing >= 0) {
            copy_fd(existing, stage_file, error_pipe,
                    PublisherChildOperation::read_existing_output,
                    temporary_path);
            static_cast<void>(::close(existing));
        } else if (errno != ENOENT) {
            report_publisher_failure(
                error_pipe, PublisherChildOperation::read_existing_output,
                errno, temporary_path);
        }
    }

    // the source fd was opened by the daemon before fork. the owner can read
    // bytes from that inherited fd without gaining path access to the private
    // spool directory itself.
    copy_fd(spool_file, stage_file, error_pipe,
            PublisherChildOperation::read_spool, temporary_path);

    if (::fsync(stage_file) < 0) {
        report_publisher_failure(
            error_pipe, PublisherChildOperation::finish_stage_file, errno,
            temporary_path);
    }
    if (::close(stage_file) < 0) {
        report_publisher_failure(
            error_pipe, PublisherChildOperation::finish_stage_file, errno,
            temporary_path);
    }

    // rename is performed as the owner too. directory permissions, sticky
    // bits, and ordinary unix ownership rules all get their normal say.
    if (::rename(temporary_path, destination.c_str()) < 0) {
        report_publisher_failure(
            error_pipe, PublisherChildOperation::publish_output, errno,
            temporary_path);
    }

    ::_exit(0);
}

[[nodiscard]] std::string_view
publisher_operation_name(PublisherChildOperation operation) {
    switch (operation) {
    case PublisherChildOperation::set_supplementary_groups:
        return "set supplementary groups";
    case PublisherChildOperation::set_group_id:
        return "set gid";
    case PublisherChildOperation::set_user_id:
        return "set uid";
    case PublisherChildOperation::create_stage_file:
        return "create temporary output";
    case PublisherChildOperation::read_existing_output:
        return "read existing output";
    case PublisherChildOperation::read_spool:
        return "read output spool";
    case PublisherChildOperation::write_stage_file:
        return "write temporary output";
    case PublisherChildOperation::finish_stage_file:
        return "finish temporary output";
    case PublisherChildOperation::publish_output:
        return "publish output";
    }

    return "publish output";
}

[[nodiscard]] OutputSpoolOperation
output_operation(PublisherChildOperation operation) {
    switch (operation) {
    case PublisherChildOperation::set_supplementary_groups:
    case PublisherChildOperation::set_group_id:
    case PublisherChildOperation::set_user_id:
        return OutputSpoolOperation::switch_owner;
    case PublisherChildOperation::create_stage_file:
        return OutputSpoolOperation::create_stage_file;
    case PublisherChildOperation::read_existing_output:
    case PublisherChildOperation::read_spool:
        return OutputSpoolOperation::read_output;
    case PublisherChildOperation::write_stage_file:
    case PublisherChildOperation::finish_stage_file:
        return OutputSpoolOperation::write_output;
    case PublisherChildOperation::publish_output:
        return OutputSpoolOperation::publish_output;
    }

    return OutputSpoolOperation::publish_output;
}

[[nodiscard]]
std::expected<std::optional<PublisherChildFailure>, OutputSpoolError>
read_publisher_failure(int error_pipe) {
    PublisherChildFailure failure{};
    auto* bytes = reinterpret_cast<char*>(&failure);
    std::size_t received = 0;

    while (received < sizeof(failure)) {
        const auto result =
            ::read(error_pipe, bytes + received, sizeof(failure) - received);

        if (result > 0) {
            received += static_cast<std::size_t>(result);
            continue;
        }
        if (result == 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }

        return std::unexpected{
            error(OutputSpoolOperation::create_pipe,
                  "could not read output publisher status", errno)};
    }

    if (received == 0) {
        return std::nullopt;
    }
    if (received != sizeof(failure)) {
        return std::unexpected{
            error(OutputSpoolOperation::create_pipe,
                  "output publisher returned a partial error", EIO)};
    }

    return failure;
}

[[nodiscard]] std::expected<void, OutputSpoolError>
publish_as_owner(const std::filesystem::path& source,
                 const std::filesystem::path& destination, bool append_output,
                 JobOwner owner) {
    auto identity = prepare_identity(owner);

    if (!identity) {
        auto identity_error = std::move(identity.error());
        return std::unexpected{
            error(OutputSpoolOperation::prepare_owner,
                  std::move(identity_error.message),
                  identity_error.system_error)};
    }

    // open the daemon-private source before fork. only this fd crosses into the
    // owner child; the spool directory itself stays inaccessible.
    OwnedFd spool_file{::open(source.c_str(), O_RDONLY | O_CLOEXEC)};

    if (spool_file.get() < 0) {
        return std::unexpected{
            error(OutputSpoolOperation::read_output,
                  "could not open output spool " + source.string(), errno)};
    }

    auto parent = destination.parent_path();

    if (parent.empty()) {
        parent = ".";
    }

    // build every string before fork. mkstemp only mutates the trailing xxxxxx
    // bytes in the child, so no c++ allocation is needed after the split.
    auto pattern =
        (parent / (".rlbs-stage-" + destination.filename().string() +
                   "-XXXXXX"))
            .string();
    std::vector<char> temporary_pattern(pattern.begin(), pattern.end());
    temporary_pattern.push_back('\0');
    std::array<int, 2> error_pipe{};

    if (::pipe2(error_pipe.data(), O_CLOEXEC) < 0) {
        return std::unexpected{
            error(OutputSpoolOperation::create_pipe,
                  "could not create output publisher status pipe", errno)};
    }

    OwnedFd pipe_read{error_pipe[0]};
    OwnedFd pipe_write{error_pipe[1]};
    const pid_t child = ::fork();

    if (child < 0) {
        return std::unexpected{
            error(OutputSpoolOperation::fork_publisher,
                  "could not fork output publisher", errno)};
    }

    if (child == 0) {
        pipe_read.close();
        publish_child(pipe_write.get(), spool_file.get(), destination,
                      temporary_pattern.data(), append_output, *identity);
    }

    // only the child reports on this end. closing our copy is what lets a
    // successful child produce eof instead of making this read wait forever.
    pipe_write.close();
    auto child_failure = read_publisher_failure(pipe_read.get());
    int status = 0;
    pid_t waited = -1;

    do {
        waited = ::waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);

    if (waited < 0) {
        return std::unexpected{
            error(OutputSpoolOperation::wait_publisher,
                  "could not wait for output publisher", errno)};
    }
    if (!child_failure) {
        return std::unexpected{std::move(child_failure.error())};
    }
    if (*child_failure) {
        const auto& failure = **child_failure;
        return std::unexpected{
            error(output_operation(failure.operation),
                  "could not " +
                      std::string{publisher_operation_name(failure.operation)} +
                      " for " + destination.string(),
                  failure.system_error)};
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return std::unexpected{
            error(OutputSpoolOperation::wait_publisher,
                  "output publisher exited without a useful error for " +
                      destination.string(),
                  EIO)};
    }

    return {};
}

[[nodiscard]] std::filesystem::path default_output_path(const Job& job,
                                                        char stream) {
    std::string name = job.spec.name.empty() ? "job" : job.spec.name;

    // names are display text, not sneaky little paths. explicit -o/-e still
    // get their real path handling below
    std::replace(name.begin(), name.end(), '/', '_');
    return name + '.' + stream + std::to_string(job.id);
}

[[nodiscard]] std::filesystem::path
resolve_destination(const Job& job,
                    const std::optional<std::filesystem::path>& requested,
                    char stream) {
    const auto path = requested.value_or(default_output_path(job, stream));
    return (path.is_absolute() ? path : job.spec.working_directory / path)
        .lexically_normal();
}

} // namespace

std::expected<PreparedOutputSpool, OutputSpoolError>
PreparedOutputSpool::create(const std::filesystem::path& spool_root,
                            const Job& job) {
    if (!job.owner) {
        return std::unexpected{
            error(OutputSpoolOperation::prepare_owner,
                  "cannot prepare output for a job without an owner", EINVAL)};
    }

    std::error_code filesystem_error;
    std::filesystem::create_directories(spool_root, filesystem_error);

    if (filesystem_error) {
        return std::unexpected{
            error(OutputSpoolOperation::create_directory,
                  "could not create output spool root " + spool_root.string() +
                      ": " + filesystem_error.message(),
                  filesystem_error.value())};
    }

    auto pattern =
        (spool_root / ("job-" + std::to_string(job.id) + "-XXXXXX")).string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');

    // one private directory per launch keeps two jobs with the same output
    // names from stepping on each other before staging
    const auto* created = ::mkdtemp(writable.data());

    if (created == nullptr) {
        return std::unexpected{
            error(OutputSpoolOperation::create_directory,
                  "could not create output spool for job " +
                      std::to_string(job.id),
                  errno)};
    }

    const std::filesystem::path directory{created};
    const auto stdout_destination =
        resolve_destination(job, job.spec.stdout_path, 'o');
    const auto stderr_destination =
        resolve_destination(job, job.spec.stderr_path, 'e');
    const bool shared_output = stdout_destination == stderr_destination;
    const auto stdout_spool = directory / "stdout";
    const auto stderr_spool =
        shared_output ? stdout_spool : directory / "stderr";

    // create both files now, before launch. even a failure before fork/exec can
    // then stage the same empty outputs instead of falling into a special case
    {
        std::ofstream stdout_file{stdout_spool,
                                  std::ios::binary | std::ios::trunc};

        if (!stdout_file) {
            return std::unexpected{
                error(OutputSpoolOperation::write_output,
                      "could not create stdout spool " +
                          stdout_spool.string(),
                      errno)};
        }
    }
    if (!shared_output) {
        std::ofstream stderr_file{stderr_spool,
                                  std::ios::binary | std::ios::trunc};

        if (!stderr_file) {
            return std::unexpected{
                error(OutputSpoolOperation::write_output,
                      "could not create stderr spool " +
                          stderr_spool.string(),
                      errno)};
        }
    }

    return PreparedOutputSpool{
        directory,
        stdout_spool,
        stderr_spool,
        stdout_destination,
        stderr_destination,
        *job.owner,
        job.spec.append_output,
        shared_output,
    };
}

PreparedOutputSpool::PreparedOutputSpool(
    std::filesystem::path directory, std::filesystem::path stdout_spool,
    std::filesystem::path stderr_spool,
    std::filesystem::path stdout_destination,
    std::filesystem::path stderr_destination, JobOwner owner,
    bool append_output, bool shared_output)
    : directory_{std::move(directory)},
      stdout_spool_{std::move(stdout_spool)},
      stderr_spool_{std::move(stderr_spool)},
      stdout_destination_{std::move(stdout_destination)},
      stderr_destination_{std::move(stderr_destination)},
      owner_{owner},
      append_output_{append_output},
      shared_output_{shared_output} {}

const std::filesystem::path& PreparedOutputSpool::stdout_path() const {
    return stdout_spool_;
}

const std::filesystem::path& PreparedOutputSpool::stderr_path() const {
    return stderr_spool_;
}

const std::filesystem::path& PreparedOutputSpool::directory() const {
    return directory_;
}

std::expected<void, OutputSpoolError>
PreparedOutputSpool::stage_one(const std::filesystem::path& source,
                               const std::filesystem::path& destination) {
    return publish_as_owner(source, destination, append_output_, owner_);
}

std::expected<void, OutputSpoolError> PreparedOutputSpool::stage() {
    if (!stdout_staged_) {
        if (auto staged = stage_one(stdout_spool_, stdout_destination_);
            !staged) {
            return staged;
        }

        stdout_staged_ = true;

        if (shared_output_) {
            stderr_staged_ = true;
        }
    }

    if (!stderr_staged_) {
        if (auto staged = stage_one(stderr_spool_, stderr_destination_);
            !staged) {
            return staged;
        }

        stderr_staged_ = true;
    }

    std::error_code remove_error;
    std::filesystem::remove_all(directory_, remove_error);

    if (remove_error) {
        return std::unexpected{
            error(OutputSpoolOperation::clean_spool,
                  "could not remove empty output spool " +
                      directory_.string() + ": " + remove_error.message(),
                  remove_error.value())};
    }

    return {};
}

} // namespace rlbs
