#pragma once

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <sys/types.h>

#include <rlbs/core/job_result.hpp>
#include <rlbs/core/job_owner.hpp>
#include <rlbs/core/job_spec.hpp>

namespace rlbs {

struct ProcessSpec {
    std::vector<std::string> argv;
    std::optional<std::filesystem::path> working_directory;
    std::vector<EnvironmentVariable> environment;
    bool inherit_environment{true};
    std::optional<std::filesystem::path> stdout_path;
    std::optional<std::filesystem::path> stderr_path;
    bool append_output{false};
    // this comes from the daemon's socket credentials, never from job text.
    // nullopt keeps the runner useful for trusted internal callers and tests.
    std::optional<JobOwner> run_as;
    // zero means leave the address-space limit alone. a nonzero value is
    // applied in the child before exec and inherited by python worker threads.
    std::optional<std::uint64_t> memory_limit_mb;
};

// keep the runner name readable while storing the exact same type on a job
using ProcessResult = JobResult;

enum class ProcessOperation {
    validate,
    validate_handle,
    inspect_working_directory,
    resolve_identity,
    open_output,
    create_pipe,
    fork_process,
    create_process_group,
    set_supplementary_groups,
    set_group_id,
    set_user_id,
    change_working_directory,
    redirect_output,
    set_memory_limit,
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
