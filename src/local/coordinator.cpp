#include <rlbs/local/coordinator.hpp>

#include <algorithm>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace rlbs {
namespace {

constexpr auto cancellation_grace_period = std::chrono::seconds{2};

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

[[nodiscard]] std::filesystem::path default_output_path(const Job& job,
                                                        char stream) {
    std::string name = job.spec.name.empty() ? "job" : job.spec.name;

    // socket clients do not get to smuggle directories into a default filename
    // through the display name. explicit output paths still work as requested
    std::ranges::replace(name, '/', '_');
    return name + '.' + stream + std::to_string(job.id);
}

[[nodiscard]] ProcessSpec process_spec(const Job& job) {
    return {
        .argv = job.spec.argv,
        .working_directory = job.spec.working_directory,
        .environment = job.spec.environment,
        .inherit_environment = job.spec.inherit_environment,
        .stdout_path =
            job.spec.stdout_path.value_or(default_output_path(job, 'o')),
        .stderr_path =
            job.spec.stderr_path.value_or(default_output_path(job, 'e')),
        .append_output = job.spec.append_output,
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

[[nodiscard]] std::string_view terminal_state_name(JobState state) {
    // cancel checks is_terminal first, so this helper only has three real
    // answers. "unknown" keeps a future enum addition from lying in an error
    switch (state) {
    case JobState::completed:
        return "completed";
    case JobState::failed:
        return "failed";
    case JobState::cancelled:
        return "cancelled";
    case JobState::pending:
    case JobState::assigned:
    case JobState::starting:
    case JobState::running:
        return "unknown";
    }

    return "unknown";
}

} // namespace

LocalCoordinator::LocalCoordinator(JobRepository& repository, Node local_node,
                                   const SchedulingPolicy& scheduler,
                                   Logger* logger)
    : repository_{repository}, scheduler_{scheduler}, logger_{logger} {
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

std::expected<void, LocalCoordinatorError>
LocalCoordinator::cancel(JobId job_id) {
    auto stored = repository_.find(job_id);

    if (!stored) {
        return std::unexpected{repository_failure(
            LocalCoordinatorOperation::load_job, std::move(stored.error()))};
    }
    if (!*stored) {
        return std::unexpected{LocalCoordinatorError{
            .operation = LocalCoordinatorOperation::load_job,
            .message = "job " + std::to_string(job_id) + " was not found",
            .repository_error = std::nullopt,
            .process_error = std::nullopt,
        }};
    }
    if (is_terminal((*stored)->state)) {
        return std::unexpected{LocalCoordinatorError{
            .operation = LocalCoordinatorOperation::load_job,
            .message = "job " + std::to_string(job_id) + " is already " +
                       std::string{terminal_state_name((*stored)->state)},
            .repository_error = std::nullopt,
            .process_error = std::nullopt,
        }};
    }

    if ((*stored)->state == JobState::pending) {
        auto cancelled = repository_.transition(
            job_id, {
                        .state = JobState::cancelled,
                        .assigned_node = std::nullopt,
                        .result = std::nullopt,
                        .detail = "cancelled before local launch",
                    });

        if (!cancelled) {
            return std::unexpected{
                repository_failure(LocalCoordinatorOperation::persist_cancelled,
                                   std::move(cancelled.error()))};
        }

        if (logger_ != nullptr) {
            logger_->info("scheduler", "job " + std::to_string(job_id) +
                                           " cancelled while pending");
        }

        return {};
    }

    auto active = std::ranges::find(active_jobs_, job_id, &ActiveJob::job_id);

    if (active == active_jobs_.end()) {
        return std::unexpected{LocalCoordinatorError{
            .operation = LocalCoordinatorOperation::load_job,
            .message = "job " + std::to_string(job_id) +
                       " is not active on the local node",
            .repository_error = std::nullopt,
            .process_error = std::nullopt,
        }};
    }

    // repeated cancel requests before the next reap are harmless. the first
    // one already told the whole process group to stop, no need to spam it
    if (active->cancellation_requested) {
        return {};
    }

    if (auto terminated = runner_.terminate(active->process); !terminated) {
        return std::unexpected{
            process_failure(LocalCoordinatorOperation::signal_cancellation,
                            std::move(terminated.error()))};
    }

    active->cancellation_requested = true;
    active->cancellation_requested_at = std::chrono::steady_clock::now();

    if (logger_ != nullptr) {
        logger_->info("executor", "sent sigterm to job " +
                                      std::to_string(job_id) +
                                      " process group");
    }

    return {};
}

std::expected<void, LocalCoordinatorError> LocalCoordinator::reap_finished() {
    for (auto active = active_jobs_.begin(); active != active_jobs_.end();) {
        auto result = runner_.poll(active->process);

        if (!result) {
            return std::unexpected{
                process_failure(LocalCoordinatorOperation::poll_process,
                                std::move(result.error()))};
        }

        if (!*result) {
            // sigterm is the polite request. after a short grace period, a job
            // ignoring it gets sigkill because "cancelled eventually maybe"
            // is not a particularly useful scheduler feature
            if (active->cancellation_requested &&
                !active->cancellation_forced &&
                std::chrono::steady_clock::now() -
                        active->cancellation_requested_at >=
                    cancellation_grace_period) {
                if (auto killed = runner_.force_kill(active->process);
                    !killed) {
                    return std::unexpected{process_failure(
                        LocalCoordinatorOperation::signal_cancellation,
                        std::move(killed.error()))};
                }

                active->cancellation_forced = true;

                if (logger_ != nullptr) {
                    logger_->warning("executor",
                                     "job " + std::to_string(active->job_id) +
                                         " ignored sigterm, sent sigkill");
                }
            }

            ++active;
            continue;
        }

        const auto final_state = active->cancellation_requested
                                     ? JobState::cancelled
                                     : JobState::completed;
        auto completed = repository_.transition(
            active->job_id, {
                                .state = final_state,
                                .assigned_node = std::nullopt,
                                .result = **result,
                                .detail = active->cancellation_requested
                                              ? "local process cancelled"
                                              : "local process exited",
                            });

        if (!completed) {
            // keep the finished handle and its allocation around so the next
            // tick can retry persistence instead of losing the final result
            return std::unexpected{
                repository_failure(LocalCoordinatorOperation::persist_finished,
                                   std::move(completed.error()))};
        }

        if (logger_ != nullptr) {
            std::ostringstream message;
            message << "job " << active->job_id << ' '
                    << (active->cancellation_requested ? "cancelled"
                                                       : "completed");

            if ((*result)->exit_code) {
                message << " exit_code=" << *(*result)->exit_code;
            }
            if ((*result)->terminating_signal) {
                message << " signal=" << *(*result)->terminating_signal;
            }

            logger_->info("executor", message.str());
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

    if (logger_ != nullptr) {
        std::ostringstream message;
        message << "job " << job.id << " assigned to " << assignment.node_id
                << " cpus=" << assignment.allocation.resources.cpus
                << " memory_mb=" << assignment.allocation.resources.memory_mb
                << " gpus=" << assignment.allocation.resources.gpus;
        logger_->info("scheduler", message.str());
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

    auto launched = runner_.launch(process_spec(job));

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

        if (logger_ != nullptr) {
            logger_->warning("executor",
                             "job " + std::to_string(job.id) +
                                 " launch failed: " + launched.error().context);
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

    if (logger_ != nullptr) {
        logger_->info("executor",
                      "job " + std::to_string(job.id) +
                          " started pid=" + std::to_string(launched->pid()));
    }

    active_jobs_.push_back({
        .job_id = job.id,
        .allocation = std::move(assignment.allocation),
        .process = std::move(*launched),
        .cancellation_requested = false,
        .cancellation_forced = false,
        .cancellation_requested_at = {},
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
