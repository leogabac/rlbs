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
#include <rlbs/logging/logger.hpp>
#include <rlbs/persistence/job_repository.hpp>

namespace rlbs {

enum class LocalCoordinatorOperation {
    load_pending,
    persist_assignment,
    persist_starting,
    launch_process,
    persist_running,
    poll_process,
    load_job,
    signal_cancellation,
    persist_cancelled,
    persist_finished,
    release_resources,
};

struct LocalCoordinatorError {
    LocalCoordinatorOperation operation{
        LocalCoordinatorOperation::load_pending};
    std::string message;
    std::optional<RepositoryError> repository_error;
    std::optional<ProcessError> process_error;
};

// this is the small bit gluing queue policy to local process execution. rlbsd
// will eventually own one and keep calling tick from its event loop
class LocalCoordinator {
  public:
    LocalCoordinator(JobRepository& repository, Node local_node,
                     const SchedulingPolicy& scheduler,
                     Logger* logger = nullptr);

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
        bool cancellation_requested{false};
        bool cancellation_forced{false};
        std::chrono::steady_clock::time_point cancellation_requested_at{};
    };

    [[nodiscard]] std::expected<void, LocalCoordinatorError> reap_finished();
    [[nodiscard]] std::expected<void, LocalCoordinatorError> start_next();
    [[nodiscard]] std::expected<void, LocalCoordinatorError>
    release(const ResourceAllocation& allocation);

    JobRepository& repository_;
    const SchedulingPolicy& scheduler_;
    LocalProcessRunner runner_;
    Logger* logger_{nullptr};
    std::vector<Node> nodes_;
    std::list<ActiveJob> active_jobs_;
};

} // namespace rlbs
