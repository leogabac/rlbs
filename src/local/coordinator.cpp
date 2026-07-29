#include <rlbs/local/coordinator.hpp>

#include <string>
#include <utility>

namespace rlbs {
namespace {

[[nodiscard]] LocalCoordinatorError
repository_failure(LocalCoordinatorOperation operation,
                   RepositoryError repository_error) {
    return {
        .operation = operation,
        .message = repository_error.message,
        .repository_error = std::move(repository_error),
        .process_error = std::nullopt,
    };
}

[[nodiscard]] LocalCoordinatorError
process_failure(LocalCoordinatorOperation operation,
                ProcessError process_error) {
    return {
        .operation = operation,
        .message = process_error.context,
        .repository_error = std::nullopt,
        .process_error = std::move(process_error),
    };
}

[[nodiscard]] ProcessSpec process_spec(const JobSpec& job) {
    return {
        .argv = job.argv,
        .working_directory = job.working_directory,
        .environment = job.environment,
        .inherit_environment = job.inherit_environment,
        .stdout_path = job.stdout_path,
        .stderr_path = job.stderr_path,
        .append_output = job.append_output,
    };
}

[[nodiscard]] std::string launch_failure_detail(const ProcessError& error) {
    std::string detail{"local launch failed"};

    if (!error.context.empty()) {
        detail.append(": ");
        detail.append(error.context);
    }

    return detail;
}

} // namespace

LocalCoordinator::LocalCoordinator(JobRepository& repository, Node local_node,
                                   const SchedulingPolicy& scheduler)
    : repository_{repository}, scheduler_{scheduler} {
    nodes_.push_back(std::move(local_node));
}

std::expected<void, LocalCoordinatorError>
LocalCoordinator::tick(bool start_new_jobs) {
    // finished jobs free resources first, otherwise a full node would waste a
    // whole tick pretending the next queued job still cannot fit
    if (auto reaped = reap_finished(); !reaped) {
        return reaped;
    }

    if (!start_new_jobs) {
        return {};
    }

    return start_next();
}

std::size_t LocalCoordinator::active_job_count() const {
    return active_jobs_.size();
}

const Node& LocalCoordinator::local_node() const { return nodes_.front(); }

std::expected<void, LocalCoordinatorError> LocalCoordinator::reap_finished() {
    for (auto active = active_jobs_.begin(); active != active_jobs_.end();) {
        auto result = runner_.poll(active->process);

        if (!result) {
            return std::unexpected{
                process_failure(LocalCoordinatorOperation::poll_process,
                                std::move(result.error()))};
        }

        if (!*result) {
            ++active;
            continue;
        }

        auto completed = repository_.transition(
            active->job_id, {
                                .state = JobState::completed,
                                .assigned_node = std::nullopt,
                                .result = **result,
                                .detail = "local process exited",
                            });

        if (!completed) {
            // keep the finished handle and its allocation around so the next
            // tick can retry persistence instead of losing the final result
            return std::unexpected{
                repository_failure(LocalCoordinatorOperation::persist_finished,
                                   std::move(completed.error()))};
        }

        if (auto released = release(active->allocation); !released) {
            return released;
        }

        active = active_jobs_.erase(active);
    }

    return {};
}

std::expected<void, LocalCoordinatorError> LocalCoordinator::start_next() {
    auto pending = repository_.pending();

    if (!pending) {
        return std::unexpected{
            repository_failure(LocalCoordinatorOperation::load_pending,
                               std::move(pending.error()))};
    }

    if (pending->empty()) {
        return {};
    }

    // only hand the oldest job to the scheduler on each tick. rlbsd will tick
    // continuously, and this keeps one broken launch from leaving a pile of
    // allocations we now have to unwind backward
    pending->resize(1);
    auto assignments = scheduler_.schedule(*pending, nodes_);

    if (assignments.empty()) {
        return {};
    }

    auto& job = pending->front();
    auto& assignment = assignments.front();

    auto assigned = repository_.transition(
        job.id, {
                    .state = JobState::assigned,
                    .assigned_node = assignment.node_id,
                    .result = std::nullopt,
                    .detail = "first-fit picked the local node",
                });

    if (!assigned) {
        static_cast<void>(release(assignment.allocation));
        return std::unexpected{
            repository_failure(LocalCoordinatorOperation::persist_assignment,
                               std::move(assigned.error()))};
    }

    auto starting =
        repository_.transition(job.id, {
                                           .state = JobState::starting,
                                           .assigned_node = std::nullopt,
                                           .result = std::nullopt,
                                           .detail = "starting local process",
                                       });

    if (!starting) {
        static_cast<void>(repository_.transition(
            job.id, {
                        .state = JobState::failed,
                        .assigned_node = std::nullopt,
                        .result = std::nullopt,
                        .detail = "could not store the starting state",
                    }));
        static_cast<void>(release(assignment.allocation));
        return std::unexpected{
            repository_failure(LocalCoordinatorOperation::persist_starting,
                               std::move(starting.error()))};
    }

    auto launched = runner_.launch(process_spec(job.spec));

    if (!launched) {
        auto failed = repository_.transition(
            job.id, {
                        .state = JobState::failed,
                        .assigned_node = std::nullopt,
                        .result = std::nullopt,
                        .detail = launch_failure_detail(launched.error()),
                    });
        auto released = release(assignment.allocation);

        if (!failed) {
            return std::unexpected{
                repository_failure(LocalCoordinatorOperation::launch_process,
                                   std::move(failed.error()))};
        }

        if (!released) {
            return released;
        }

        // a bad executable is a failed job, not a broken daemon tick. the queue
        // can keep moving after its failure has been recorded
        return {};
    }

    auto running =
        repository_.transition(job.id, {
                                           .state = JobState::running,
                                           .assigned_node = std::nullopt,
                                           .result = std::nullopt,
                                           .detail = "local process started",
                                       });

    if (!running) {
        // the process exists but we cannot honestly call it running in the
        // database. kill the whole group now instead of making an orphan
        static_cast<void>(runner_.force_kill(*launched));
        static_cast<void>(runner_.wait(*launched));
        static_cast<void>(repository_.transition(
            job.id, {
                        .state = JobState::failed,
                        .assigned_node = std::nullopt,
                        .result = std::nullopt,
                        .detail = "could not store the running state",
                    }));
        static_cast<void>(release(assignment.allocation));
        return std::unexpected{
            repository_failure(LocalCoordinatorOperation::persist_running,
                               std::move(running.error()))};
    }

    active_jobs_.push_back({
        .job_id = job.id,
        .allocation = std::move(assignment.allocation),
        .process = std::move(*launched),
    });
    return {};
}

std::expected<void, LocalCoordinatorError>
LocalCoordinator::release(const ResourceAllocation& allocation) {
    if (nodes_.front().release(allocation)) {
        return {};
    }

    return std::unexpected{LocalCoordinatorError{
        .operation = LocalCoordinatorOperation::release_resources,
        .message = "local resource accounting rejected a release",
        .repository_error = std::nullopt,
        .process_error = std::nullopt,
    }};
}

} // namespace rlbs
