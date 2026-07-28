#include <rlbs/execution/process_runner.hpp>

#include <array>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <string_view>
#include <utility>

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace rlbs {
namespace {

class UniqueFd {
  public:
    UniqueFd() = default;
    explicit UniqueFd(int fd) : fd_{fd} {}

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    UniqueFd(UniqueFd&& other) noexcept : fd_{std::exchange(other.fd_, -1)} {}

    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, -1);
        }

        return *this;
    }

    ~UniqueFd() { reset(); }

    [[nodiscard]] int get() const { return fd_; }

    void reset(int fd = -1) {
        if (fd_ >= 0) {
            static_cast<void>(::close(fd_));
        }

        fd_ = fd;
    }

  private:
    int fd_{-1};
};

enum class ChildOperation : int {
    create_process_group,
    change_working_directory,
    redirect_output,
    execute,
};

struct ChildFailure {
    ChildOperation operation;
    int system_error;
};

[[nodiscard]] ProcessError error(ProcessOperation operation, int system_error,
                                 std::string context) {
    return {
        .operation = operation,
        .system_error = system_error,
        .context = std::move(context),
    };
}

[[nodiscard]] bool contains_null(std::string_view value) {
    return value.find('\0') != std::string_view::npos;
}

[[nodiscard]] std::expected<void, ProcessError>
validate(const ProcessSpec& spec) {
    if (spec.argv.empty() || spec.argv.front().empty()) {
        return std::unexpected{
            error(ProcessOperation::validate, EINVAL, "argv cannot be empty")};
    }

    for (const auto& argument : spec.argv) {
        if (contains_null(argument)) {
            return std::unexpected{error(ProcessOperation::validate, EINVAL,
                                         "argv contains an embedded null")};
        }
    }

    for (const auto& variable : spec.environment) {
        if (variable.name.empty() || variable.name.contains('=') ||
            contains_null(variable.name) || contains_null(variable.value)) {
            return std::unexpected{error(ProcessOperation::validate, EINVAL,
                                         "environment variable is malformed")};
        }
    }

    for (const auto* path :
         {&spec.working_directory, &spec.stdout_path, &spec.stderr_path}) {
        if (*path && contains_null((*path)->native())) {
            return std::unexpected{
                error(ProcessOperation::validate, EINVAL,
                      "filesystem path contains an embedded null")};
        }
    }

    return {};
}

[[nodiscard]] std::expected<std::filesystem::path, ProcessError>
working_directory(const ProcessSpec& spec) {
    std::error_code filesystem_error;
    auto directory = spec.working_directory
                         ? std::filesystem::absolute(*spec.working_directory,
                                                     filesystem_error)
                         : std::filesystem::current_path(filesystem_error);

    if (filesystem_error) {
        return std::unexpected{
            error(ProcessOperation::inspect_working_directory,
                  filesystem_error.value(),
                  spec.working_directory ? spec.working_directory->string()
                                         : std::string{})};
    }

    const bool is_directory =
        std::filesystem::is_directory(directory, filesystem_error);

    if (filesystem_error || !is_directory) {
        return std::unexpected{
            error(ProcessOperation::inspect_working_directory,
                  filesystem_error ? filesystem_error.value() : ENOTDIR,
                  directory.string())};
    }

    return directory;
}

[[nodiscard]] std::filesystem::path
resolve_output_path(const std::optional<std::filesystem::path>& path,
                    const std::filesystem::path& directory) {
    if (!path) {
        return {};
    }

    if (path->is_absolute()) {
        return path->lexically_normal();
    }

    return (directory / *path).lexically_normal();
}

[[nodiscard]] std::expected<UniqueFd, ProcessError>
open_output(const std::filesystem::path& path, bool append) {
    if (path.empty()) {
        return UniqueFd{};
    }

    const int flags =
        O_WRONLY | O_CREAT | O_CLOEXEC | (append ? O_APPEND : O_TRUNC);
    const int fd = ::open(path.c_str(), flags, 0666);

    if (fd < 0) {
        return std::unexpected{
            error(ProcessOperation::open_output, errno, path.string())};
    }

    return UniqueFd{fd};
}

[[nodiscard]] std::map<std::string, std::string>
build_environment(const ProcessSpec& spec) {
    std::map<std::string, std::string> environment;

    if (spec.inherit_environment) {
        for (char** entry = environ; entry != nullptr && *entry != nullptr;
             ++entry) {
            const std::string_view value{*entry};
            const auto separator = value.find('=');

            if (separator != std::string_view::npos) {
                environment.insert_or_assign(
                    std::string{value.substr(0, separator)},
                    std::string{value.substr(separator + 1)});
            }
        }
    }

    for (const auto& variable : spec.environment) {
        environment.insert_or_assign(variable.name, variable.value);
    }

    return environment;
}

[[nodiscard]] std::vector<std::string> build_environment_storage(
    const std::map<std::string, std::string>& environment) {
    std::vector<std::string> storage;
    storage.reserve(environment.size());

    for (const auto& [name, value] : environment) {
        std::string entry;
        entry.reserve(name.size() + value.size() + 1);
        entry.append(name);
        entry.push_back('=');
        entry.append(value);
        storage.push_back(std::move(entry));
    }

    return storage;
}

[[nodiscard]] std::vector<char*>
build_pointer_array(std::vector<std::string>& storage) {
    std::vector<char*> pointers;
    pointers.reserve(storage.size() + 1);

    for (auto& value : storage) {
        pointers.push_back(value.data());
    }

    pointers.push_back(nullptr);
    return pointers;
}

[[nodiscard]] std::vector<std::string>
executable_candidates(const std::string& executable,
                      const std::map<std::string, std::string>& environment) {
    if (executable.contains('/')) {
        return {executable};
    }

    const auto path = environment.contains("PATH")
                          ? environment.at("PATH")
                          : std::string{"/bin:/usr/bin"};
    std::vector<std::string> candidates;
    std::size_t begin = 0;

    while (begin <= path.size()) {
        const auto end = path.find(':', begin);
        const auto directory = path.substr(begin, end - begin);
        candidates.push_back(
            (directory.empty() ? std::string{"."} : directory) + '/' +
            executable);

        if (end == std::string::npos) {
            break;
        }

        begin = end + 1;
    }

    return candidates;
}

[[noreturn]] void report_child_failure(int pipe_fd, ChildOperation operation,
                                       int system_error) {
    const ChildFailure failure{
        .operation = operation,
        .system_error = system_error,
    };
    const auto* data = reinterpret_cast<const char*>(&failure);
    std::size_t written = 0;

    while (written < sizeof(failure)) {
        const auto result =
            ::write(pipe_fd, data + written, sizeof(failure) - written);

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

void redirect_fd(int source, int destination, int error_pipe) {
    if (source < 0) {
        return;
    }

    if (source != destination) {
        if (::dup2(source, destination) < 0) {
            report_child_failure(error_pipe, ChildOperation::redirect_output,
                                 errno);
        }

        return;
    }

    // open normally lands above stderr, but closed standard fds are legal and
    // then o_cloexec would quietly eat the redirected stream during exec
    const int flags = ::fcntl(destination, F_GETFD);

    if (flags < 0 || ::fcntl(destination, F_SETFD, flags & ~FD_CLOEXEC) < 0) {
        report_child_failure(error_pipe, ChildOperation::redirect_output,
                             errno);
    }
}

[[noreturn]] void execute_child(int error_pipe, int stdout_fd, int stderr_fd,
                                bool joined_output,
                                const std::filesystem::path& directory,
                                const std::vector<std::string>& candidates,
                                char* const* argv, char* const* environment) {
    if (::setpgid(0, 0) < 0) {
        report_child_failure(error_pipe, ChildOperation::create_process_group,
                             errno);
    }

    if (::chdir(directory.c_str()) < 0) {
        report_child_failure(error_pipe,
                             ChildOperation::change_working_directory, errno);
    }

    redirect_fd(stdout_fd, STDOUT_FILENO, error_pipe);

    if (joined_output) {
        if (stdout_fd < 0 || ::dup2(STDOUT_FILENO, STDERR_FILENO) < 0) {
            report_child_failure(error_pipe, ChildOperation::redirect_output,
                                 errno);
        }
    } else if (stderr_fd >= 0 && ::dup2(stderr_fd, STDERR_FILENO) < 0) {
        report_child_failure(error_pipe, ChildOperation::redirect_output,
                             errno);
    }

    if (stdout_fd >= 0 && stdout_fd != STDOUT_FILENO) {
        static_cast<void>(::close(stdout_fd));
    }

    if (stderr_fd >= 0 && stderr_fd != STDERR_FILENO &&
        stderr_fd != stdout_fd) {
        static_cast<void>(::close(stderr_fd));
    }

    int execute_error = ENOENT;

    for (const auto& candidate : candidates) {
        ::execve(candidate.c_str(), argv, environment);

        if (errno == EACCES) {
            execute_error = EACCES;
            continue;
        }

        if (errno != ENOENT && errno != ENOTDIR) {
            execute_error = errno;
            break;
        }
    }

    report_child_failure(error_pipe, ChildOperation::execute, execute_error);
}

[[nodiscard]] ProcessOperation operation_from_child(ChildOperation operation) {
    switch (operation) {
    case ChildOperation::create_process_group:
        return ProcessOperation::create_process_group;
    case ChildOperation::change_working_directory:
        return ProcessOperation::change_working_directory;
    case ChildOperation::redirect_output:
        return ProcessOperation::redirect_output;
    case ChildOperation::execute:
        return ProcessOperation::execute;
    }

    return ProcessOperation::execute;
}

void reap_after_failed_launch(pid_t pid) {
    int status = 0;

    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
}

[[nodiscard]] std::expected<std::optional<ChildFailure>, ProcessError>
read_child_failure(int pipe_fd) {
    ChildFailure failure{};
    auto* data = reinterpret_cast<char*>(&failure);
    std::size_t received = 0;

    while (received < sizeof(failure)) {
        const auto result =
            ::read(pipe_fd, data + received, sizeof(failure) - received);

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
            error(ProcessOperation::create_pipe, errno, "read exec status")};
    }

    if (received == 0) {
        return std::nullopt;
    }

    if (received != sizeof(failure)) {
        return std::unexpected{error(ProcessOperation::create_pipe, EIO,
                                     "short read from exec status pipe")};
    }

    return failure;
}

[[nodiscard]] ProcessResult decode_status(int status) {
    ProcessResult result;

    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.terminating_signal = WTERMSIG(status);
#ifdef WCOREDUMP
        result.dumped_core = WCOREDUMP(status);
#endif
    }

    return result;
}

} // namespace

ProcessHandle::ProcessHandle(pid_t pid, pid_t process_group_id)
    : pid_{pid}, process_group_id_{process_group_id} {}

ProcessHandle::ProcessHandle(ProcessHandle&& other) noexcept
    : pid_{std::exchange(other.pid_, -1)},
      process_group_id_{std::exchange(other.process_group_id_, -1)},
      result_{other.result_} {
    other.result_.reset();
}

pid_t ProcessHandle::pid() const { return pid_; }

pid_t ProcessHandle::process_group_id() const { return process_group_id_; }

bool ProcessHandle::finished() const { return result_.has_value(); }

std::expected<ProcessHandle, ProcessError>
LocalProcessRunner::launch(const ProcessSpec& spec) const {
    if (auto validation = validate(spec); !validation) {
        return std::unexpected{std::move(validation.error())};
    }

    auto directory = working_directory(spec);

    if (!directory) {
        return std::unexpected{std::move(directory.error())};
    }

    const auto stdout_path = resolve_output_path(spec.stdout_path, *directory);
    const auto stderr_path = resolve_output_path(spec.stderr_path, *directory);
    const bool joined_output =
        !stdout_path.empty() && stdout_path == stderr_path;

    auto stdout_file = open_output(stdout_path, spec.append_output);

    if (!stdout_file) {
        return std::unexpected{std::move(stdout_file.error())};
    }

    auto stderr_file = joined_output
                           ? std::expected<UniqueFd, ProcessError>{UniqueFd{}}
                           : open_output(stderr_path, spec.append_output);

    if (!stderr_file) {
        return std::unexpected{std::move(stderr_file.error())};
    }

    auto environment = build_environment(spec);
    auto environment_storage = build_environment_storage(environment);
    auto environment_pointers = build_pointer_array(environment_storage);
    auto argv_storage = spec.argv;
    auto argv_pointers = build_pointer_array(argv_storage);
    const auto candidates =
        executable_candidates(spec.argv.front(), environment);

    std::array<int, 2> error_pipe{};

    if (::pipe2(error_pipe.data(), O_CLOEXEC) < 0) {
        return std::unexpected{
            error(ProcessOperation::create_pipe, errno, "exec status")};
    }

    UniqueFd pipe_read{error_pipe[0]};
    UniqueFd pipe_write{error_pipe[1]};
    const pid_t pid = ::fork();

    if (pid < 0) {
        return std::unexpected{
            error(ProcessOperation::fork_process, errno, spec.argv.front())};
    }

    if (pid == 0) {
        pipe_read.reset();
        execute_child(pipe_write.get(), stdout_file->get(), stderr_file->get(),
                      joined_output, *directory, candidates,
                      argv_pointers.data(), environment_pointers.data());
    }

    pipe_write.reset();

    // both sides call setpgid because the child can reach exec annoyingly fast.
    // either one winning is fine, we just need the group to exist before launch
    // returns and somebody tries to cancel it
    if (::setpgid(pid, pid) < 0 && errno != EACCES && errno != ESRCH) {
        const int process_group_error = errno;
        static_cast<void>(::kill(pid, SIGKILL));
        reap_after_failed_launch(pid);
        return std::unexpected{error(ProcessOperation::create_process_group,
                                     process_group_error, spec.argv.front())};
    }

    auto child_failure = read_child_failure(pipe_read.get());

    if (!child_failure) {
        static_cast<void>(::kill(-pid, SIGKILL));
        static_cast<void>(::kill(pid, SIGKILL));
        reap_after_failed_launch(pid);
        return std::unexpected{std::move(child_failure.error())};
    }

    if (*child_failure) {
        reap_after_failed_launch(pid);
        return std::unexpected{
            error(operation_from_child((*child_failure)->operation),
                  (*child_failure)->system_error, spec.argv.front())};
    }

    return ProcessHandle{pid, pid};
}

std::expected<std::optional<ProcessResult>, ProcessError>
LocalProcessRunner::poll(ProcessHandle& process) const {
    if (process.result_) {
        return process.result_;
    }

    if (process.pid_ <= 0) {
        return std::unexpected{error(ProcessOperation::validate_handle, EINVAL,
                                     "invalid process handle")};
    }

    int status = 0;
    pid_t result = 0;

    do {
        result = ::waitpid(process.pid_, &status, WNOHANG);
    } while (result < 0 && errno == EINTR);

    if (result == 0) {
        return std::nullopt;
    }

    if (result < 0) {
        return std::unexpected{
            error(ProcessOperation::wait, errno, std::to_string(process.pid_))};
    }

    process.result_ = decode_status(status);
    return process.result_;
}

std::expected<ProcessResult, ProcessError>
LocalProcessRunner::wait(ProcessHandle& process) const {
    if (process.result_) {
        return *process.result_;
    }

    if (process.pid_ <= 0) {
        return std::unexpected{error(ProcessOperation::validate_handle, EINVAL,
                                     "invalid process handle")};
    }

    int status = 0;
    pid_t result = 0;

    do {
        result = ::waitpid(process.pid_, &status, 0);
    } while (result < 0 && errno == EINTR);

    if (result < 0) {
        return std::unexpected{
            error(ProcessOperation::wait, errno, std::to_string(process.pid_))};
    }

    process.result_ = decode_status(status);
    return *process.result_;
}

std::expected<void, ProcessError>
LocalProcessRunner::terminate(const ProcessHandle& process) const {
    return signal_process_group(process, SIGTERM);
}

std::expected<void, ProcessError>
LocalProcessRunner::force_kill(const ProcessHandle& process) const {
    return signal_process_group(process, SIGKILL);
}

std::expected<void, ProcessError>
LocalProcessRunner::signal_process_group(const ProcessHandle& process,
                                         int signal) const {
    if (process.result_) {
        return {};
    }

    if (process.process_group_id_ <= 0) {
        return std::unexpected{error(ProcessOperation::signal_process_group,
                                     EINVAL, "invalid process group")};
    }

    if (::kill(-process.process_group_id_, signal) < 0 && errno != ESRCH) {
        return std::unexpected{
            error(ProcessOperation::signal_process_group, errno,
                  std::to_string(process.process_group_id_))};
    }

    return {};
}

} // namespace rlbs
