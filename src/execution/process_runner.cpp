/*
 * These pieces of code were written with help of codex
 * dealing with system calls and posix shenannigans
 * are not my area of expertise.
 *
 * Then I added my comments and explanations on top
 *
 * atte: leogabac
 */

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

// libc exposes the current process environment through this slightly ancient
// global. we copy it before fork so the child does not have to build anything
extern char** environ;

namespace rlbs {
namespace {

// - expected<thing, error> returns either the thing or an error
// - optional<thing> means the thing may not exist yet
// - ::open / ::close / ::fork are raw posix calls, not class methods
// - nodiscard asks the compiler to complain if a result gets ignored
// - static_cast<void>(call) says we are deliberately ignoring that result
// - exchange(old, -1) takes the old value and leaves -1 behind

// "unique" means exactly one object owns this fd, same idea as unique_ptr.
// copying would make two destructors close the same number, which gets ugly
// fast
// here Fd stands for "File Descriptor", apparently obvious for actual kernel devs or sth
class UniqueFd {
  public:
    // -1 means "owns nothing", so the default object is safe to destroy
    UniqueFd() = default;

    // this takes responsibility for a raw fd and will close it later
    explicit UniqueFd(int fd) : fd_{fd} {}

    // there can only be one closer for an fd, so copies are intentionally
    // banned
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    // moving hands the fd to the new owner and leaves the old one harmless
    UniqueFd(UniqueFd&& other) noexcept : fd_{std::exchange(other.fd_, -1)} {}

    // moving over an existing owner closes its old fd before taking the new one
    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, -1);
        }

        return *this;
    }

    // every return path ends up here, so forgotten close calls stop being a
    // thing
    ~UniqueFd() { reset(); }

    // callers can borrow the number for a syscall, but ownership stays here
    [[nodiscard]] int get() const { return fd_; }

    // close whatever we own now, then optionally take ownership of another fd
    void reset(int fd = -1) {
        if (fd_ >= 0) {
            static_cast<void>(::close(fd_));
        }

        fd_ = fd;
    }

  private:
    int fd_{-1};
};

// the child can only send a tiny record through the pipe, so these values label
// which pre-exec step failed without trying to ship a c++ object across
enum class ChildOperation : int {
    create_process_group,
    change_working_directory,
    redirect_output,
    execute,
};

// keep this fixed-size and boring because the child writes its raw bytes
struct ChildFailure {
    ChildOperation operation;
    int system_error;
};

// bundle the failed operation, errno, and a useful label into one consistent
// error instead of rebuilding the same little object at every unhappy return
[[nodiscard]] ProcessError error(ProcessOperation operation, int system_error,
                                 std::string context) {
    return {
        .operation = operation,
        .system_error = system_error,
        .context = std::move(context),
    };
}

// exec-style apis use null-terminated strings, so an embedded null would make
// the kernel see only half the value and leave us debugging a very fake mystery
[[nodiscard]] bool contains_null(std::string_view value) {
    return value.find('\0') != std::string_view::npos;
}

// check the whole request while we are still safely in the parent. spec carries
// argv, environment changes, working directory, and output paths into launch
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

// turn the requested working directory into a checked absolute path. if none
// was requested, the job inherits wherever rlbs is currently standing
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

// output paths are interpreted from the job directory, not from some random
// directory the daemon happened to start in
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

// open one output file and return its unique owner. append selects whether old
// output survives; an empty path means the child just inherits the current fd
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

// build the exact environment the child should receive. inherited values go in
// first, then spec overrides win because otherwise overrides would be
// decorative
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

// execve wants each variable as one "name=value" string, so flatten the nicer
// map into storage that stays alive until fork and exec are finished with it
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

// execve also wants old-school char** arrays ending in nullptr. these pointers
// borrow the strings above, so the storage must not move or disappear afterward
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

// if argv[0] has no slash, expand every path entry into a candidate executable.
// doing this before fork keeps string allocation out of the child-side code
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

// the child cannot return a normal c++ error after fork, so write a tiny fixed
// record to the parent and exit immediately. pipe_fd is the private error pipe
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

// point one standard stream at an already-open fd. error_pipe is only here so a
// failed dup can still be explained to the parent instead of vanishing silently
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

// this is the child-only half of launch. it creates the job process group,
// changes directory, wires output, and finally replaces itself with argv[0]
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

// child failures use a tiny internal enum that is safe to send through a pipe.
// convert it back to the public operation names the rest of rlbs understands
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

// if launch fails after fork, wait for that child here so it does not sit
// around as a zombie just because setup already went sideways
void reap_after_failed_launch(pid_t pid) {
    int status = 0;

    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
}

// read the child's pre-exec status pipe. eof with no bytes means exec
// succeeded; a full record means setup failed, and a partial record means
// something broke
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

// waitpid gives us one packed integer full of macros. unpack it into either an
// exit code or a signal so callers never need to learn this particular nonsense
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

// a handle remembers both the child pid we wait on and the process-group id we
// signal. they match at launch, but they represent two different jobs here
ProcessHandle::ProcessHandle(pid_t pid, pid_t process_group_id)
    : pid_{pid}, process_group_id_{process_group_id} {}

// transfer the right to wait on this child. the old handle gets -1 so using it
// cannot accidentally turn into waitpid(-1) and reap some completely unrelated
// job
ProcessHandle::ProcessHandle(ProcessHandle&& other) noexcept
    : pid_{std::exchange(other.pid_, -1)},
      process_group_id_{std::exchange(other.process_group_id_, -1)},
      result_{other.result_} {
    other.result_.reset();
}

// expose the child pid for logging and later persistence, not for ownership
pid_t ProcessHandle::pid() const { return pid_; }

// expose the group id used to signal the job and any children it spawned
pid_t ProcessHandle::process_group_id() const { return process_group_id_; }

// once a result is cached, the child has already been reaped and wait is done
bool ProcessHandle::finished() const { return result_.has_value(); }

// prepare every fallible bit in the parent, fork once, then wait only long
// enough to know exec succeeded. spec contains argv, environment, cwd, and
// output
std::expected<ProcessHandle, ProcessError>
LocalProcessRunner::launch(const ProcessSpec& spec) const {
    if (auto validation = validate(spec); !validation) {
        return std::unexpected{std::move(validation.error())};
    }

    auto directory = working_directory(spec);

    if (!directory) {
        return std::unexpected{std::move(directory.error())};
    }

    // resolve and open output before fork so ordinary filesystem errors come
    // back as ordinary errors, not cryptic messages from a half-created child
    const auto stdout_path = resolve_output_path(spec.stdout_path, *directory);
    const auto stderr_path = resolve_output_path(spec.stderr_path, *directory);

    // one shared fd keeps stdout and stderr correctly interleaved when both
    // names point at the same file instead of opening and truncating it twice
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

    // all this storage has to exist before fork because argv and envp are
    // pointer arrays borrowing memory from these strings
    auto environment = build_environment(spec);
    auto environment_storage = build_environment_storage(environment);
    auto environment_pointers = build_pointer_array(environment_storage);
    auto argv_storage = spec.argv;
    auto argv_pointers = build_pointer_array(argv_storage);
    const auto candidates =
        executable_candidates(spec.argv.front(), environment);

    // this pipe is the launch handshake. cloexec closes the child end only when
    // exec succeeds, so the parent can tell success from "fork worked, exec did
    // not"
    std::array<int, 2> error_pipe{};

    if (::pipe2(error_pipe.data(), O_CLOEXEC) < 0) {
        return std::unexpected{
            error(ProcessOperation::create_pipe, errno, "exec status")};
    }

    UniqueFd pipe_read{error_pipe[0]};
    UniqueFd pipe_write{error_pipe[1]};

    // fork returns twice: pid 0 continues as the child, while the positive pid
    // keeps the parent on the scheduler side of the split
    const pid_t pid = ::fork();

    if (pid < 0) {
        return std::unexpected{
            error(ProcessOperation::fork_process, errno, spec.argv.front())};
    }

    if (pid == 0) {
        // the child writes failures, so keeping the read end open serves no one
        pipe_read.reset();
        execute_child(pipe_write.get(), stdout_file->get(), stderr_file->get(),
                      joined_output, *directory, candidates,
                      argv_pointers.data(), environment_pointers.data());
    }

    // the parent only reads failures. leaving its write end open would prevent
    // eof forever and make a successful launch hang here looking very stupid
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

    // this blocks only until exec either succeeds or reports which setup step
    // failed; it does not wait for the actual job to finish
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

// ask waitpid without blocking. nullopt means the child is still running, while
// a process result means it exited and has now been reaped exactly once
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

// block until the child exits, then cache the decoded status on the handle.
// repeated calls return that cache because waitpid cannot reap the same child
// twice
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

// send the polite cancellation signal to the entire job process group
std::expected<void, ProcessError>
LocalProcessRunner::terminate(const ProcessHandle& process) const {
    return signal_process_group(process, SIGTERM);
}

// send sigkill when the polite version was ignored and we are done negotiating
std::expected<void, ProcessError>
LocalProcessRunner::force_kill(const ProcessHandle& process) const {
    return signal_process_group(process, SIGKILL);
}

// both cancellation methods end up here. the negative group id is how kill()
// targets the whole job tree instead of only the original child process
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
