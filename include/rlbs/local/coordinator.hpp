#pragma once

#include <chrono>
#include <cstddef>
#include <expected>
#include <list>
#include <optional>
#include <string>
#include <vector>

#include <rlbs/core/node.hpp>
#include <rlbs/core/scheduler.hpp>
#include <rlbs/execution/process_runner.hpp>
#include <rlbs/execution/cgroup.hpp>
#include <rlbs/local/output_spool.hpp>
#include <rlbs/local/runtime_environment.hpp>
#include <rlbs/logging/logger.hpp>
#include <rlbs/persistence/job_repository.hpp>
#include <rlbs/persistence/queue_repository.hpp>

namespace rlbs {

enum class LocalCoordinatorOperation {
    load_jobs,
    load_queues,
    persist_assignment,
    persist_starting,
    prepare_runtime_environment,
    prepare_output_spool,
    launch_process,
    persist_running,
    poll_process,
    load_job,
    signal_cancellation,
    persist_cancelled,
    persist_finished,
    stage_output,
    release_resources,
};

struct LocalCoordinatorError {
    LocalCoordinatorOperation operation{
        LocalCoordinatorOperation::load_jobs};
    std::string message;
    std::optional<RepositoryError> repository_error;
    std::optional<ProcessError> process_error;
    std::optional<RuntimeEnvironmentError> runtime_environment_error;
    std::optional<OutputSpoolError> output_spool_error;
};

// this is the small bit gluing queue policy to local process execution. rlbsd
// will eventually own one and keep calling tick from its event loop
class LocalCoordinator {
  public:
    LocalCoordinator(JobRepository& repository, QueueRepository& queues,
                     Node local_node,
                     const SchedulingPolicy& scheduler,
                     std::filesystem::path spool_directory,
                     Logger* logger = nullptr,
                     std::filesystem::path cgroup_root = {});

    [[nodiscard]] std::expected<void, LocalCoordinatorError>
    tick(bool start_new_jobs = true);

    [[nodiscard]] std::expected<void, LocalCoordinatorError>
    cancel(JobId job_id);

    [[nodiscard]] std::size_t active_job_count() const;
    [[nodiscard]] const Node& local_node() const;

  private:
    struct ActiveJob {
        JobId job_id;
        ResourceAllocation allocation;
        ProcessHandle process;
        PreparedRuntimeEnvironment runtime_environment;
        PreparedOutputSpool output_spool;
        std::optional<std::chrono::seconds> walltime;
        std::chrono::steady_clock::time_point started_at;
        bool cancellation_requested{false};
        bool cancellation_forced{false};
        bool walltime_exceeded{false};
        std::optional<JobCgroup> cgroup;
        std::chrono::steady_clock::time_point cancellation_requested_at{};
    };

    [[nodiscard]] std::expected<void, LocalCoordinatorError> reap_finished();
    [[nodiscard]] std::expected<void, LocalCoordinatorError> start_next();
    [[nodiscard]] std::expected<void, LocalCoordinatorError>
    release(const ResourceAllocation& allocation);

    JobRepository& repository_;
    QueueRepository& queues_;
    const SchedulingPolicy& scheduler_;
    LocalProcessRunner runner_;
    Logger* logger_{nullptr};
    std::filesystem::path spool_directory_;
    std::filesystem::path cgroup_root_;
    std::vector<Node> nodes_;
    std::list<ActiveJob> active_jobs_;
};

} // namespace rlbs
