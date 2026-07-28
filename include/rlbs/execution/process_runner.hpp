#pragma once

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <sys/types.h>

namespace rlbs {

struct EnvironmentVariable {
    std::string name;
    std::string value;
};

struct ProcessSpec {
    std::vector<std::string> argv;
    std::optional<std::filesystem::path> working_directory;
    std::vector<EnvironmentVariable> environment;
    bool inherit_environment{true};
    std::optional<std::filesystem::path> stdout_path;
    std::optional<std::filesystem::path> stderr_path;
    bool append_output{false};
};

struct ProcessResult {
    std::optional<int> exit_code;
    std::optional<int> terminating_signal;
    bool dumped_core{false};
};

enum class ProcessOperation {
    validate,
    validate_handle,
    inspect_working_directory,
    open_output,
    create_pipe,
    fork_process,
    create_process_group,
    change_working_directory,
    redirect_output,
    execute,
    wait,
    signal_process_group,
};

struct ProcessError {
    ProcessOperation operation{ProcessOperation::validate};
    int system_error{0};
    std::string context;
};

class ProcessHandle {
  public:
    ProcessHandle(const ProcessHandle&) = delete;
    ProcessHandle& operator=(const ProcessHandle&) = delete;
    ProcessHandle(ProcessHandle&& other) noexcept;
    ProcessHandle& operator=(ProcessHandle&&) = delete;

    [[nodiscard]] pid_t pid() const;
    [[nodiscard]] pid_t process_group_id() const;
    [[nodiscard]] bool finished() const;

  private:
    friend class LocalProcessRunner;

    ProcessHandle(pid_t pid, pid_t process_group_id);

    pid_t pid_{-1};
    pid_t process_group_id_{-1};
    std::optional<ProcessResult> result_;
};

class LocalProcessRunner {
  public:
    [[nodiscard]] std::expected<ProcessHandle, ProcessError>
    launch(const ProcessSpec& spec) const;

    [[nodiscard]] std::expected<std::optional<ProcessResult>, ProcessError>
    poll(ProcessHandle& process) const;

    [[nodiscard]] std::expected<ProcessResult, ProcessError>
    wait(ProcessHandle& process) const;

    [[nodiscard]] std::expected<void, ProcessError>
    terminate(const ProcessHandle& process) const;

    [[nodiscard]] std::expected<void, ProcessError>
    force_kill(const ProcessHandle& process) const;

  private:
    [[nodiscard]] std::expected<void, ProcessError>
    signal_process_group(const ProcessHandle& process, int signal) const;
};

} // namespace rlbs
