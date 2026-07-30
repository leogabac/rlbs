#include <rlbs/local/coordinator.hpp>

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
        .runtime_environment_error = std::nullopt,
        .output_spool_error = std::nullopt,
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
        .runtime_environment_error = std::nullopt,
        .output_spool_error = std::nullopt,
    };
}

[[nodiscard]] LocalCoordinatorError
runtime_environment_failure(RuntimeEnvironmentError runtime_error) {
    return {
        .operation = LocalCoordinatorOperation::prepare_runtime_environment,
        .message = runtime_error.message,
        .repository_error = std::nullopt,
        .process_error = std::nullopt,
        .runtime_environment_error = std::move(runtime_error),
        .output_spool_error = std::nullopt,
    };
}

[[nodiscard]] LocalCoordinatorError
output_spool_failure(LocalCoordinatorOperation operation,
                     OutputSpoolError spool_error) {
    return {
        .operation = operation,
        .message = spool_error.message,
        .repository_error = std::nullopt,
        .process_error = std::nullopt,
        .runtime_environment_error = std::nullopt,
        .output_spool_error = std::move(spool_error),
    };
}

[[nodiscard]] ProcessSpec
process_spec(const Job& job,
             const PreparedRuntimeEnvironment& runtime_environment,
             const PreparedOutputSpool& output_spool) {
    return {
        .argv = job.spec.argv,
        .working_directory = job.spec.working_directory,
        .environment = runtime_environment.variables(),
        .inherit_environment = job.spec.inherit_environment,
        .stdout_path = output_spool.stdout_path(),
        .stderr_path = output_spool.stderr_path(),
        // append applies when the spool is staged. each launch gets a fresh
        // private file, so appending inside it would only preserve stale junk
        .append_output = false,
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
                                   std::filesystem::path spool_directory,
                                   Logger* logger)
    : repository_{repository}, scheduler_{scheduler}, logger_{logger},
      spool_directory_{std::move(spool_directory)} {
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
            .runtime_environment_error = std::nullopt,
            .output_spool_error = std::nullopt,
        }};
    }
    if (is_terminal((*stored)->state)) {
        return std::unexpected{LocalCoordinatorError{
            .operation = LocalCoordinatorOperation::load_job,
            .message = "job " + std::to_string(job_id) + " is already " +
                       std::string{terminal_state_name((*stored)->state)},
            .repository_error = std::nullopt,
            .process_error = std::nullopt,
            .runtime_environment_error = std::nullopt,
            .output_spool_error = std::nullopt,
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
            .runtime_environment_error = std::nullopt,
            .output_spool_error = std::nullopt,
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
            const auto now = std::chrono::steady_clock::now();

            // walltime measures actual execution, not time spent waiting in the
            // queue. once it expires, use the same process-group shutdown path
            // as cancel because inventing a second signal mess would be silly
            if (!active->cancellation_requested && active->walltime &&
                now - active->started_at >= *active->walltime) {
                if (auto terminated = runner_.terminate(active->process);
                    !terminated) {
                    return std::unexpected{process_failure(
                        LocalCoordinatorOperation::signal_cancellation,
                        std::move(terminated.error()))};
                }

                active->cancellation_requested = true;
                active->walltime_exceeded = true;
                active->cancellation_requested_at = now;

                if (logger_ != nullptr) {
                    logger_->warning(
                        "executor",
                        "job " + std::to_string(active->job_id) +
                            " exceeded walltime, sent sigterm");
                }
            }

            // sigterm is the polite request. after a short grace period, a job
            // ignoring it gets sigkill because "cancelled eventually maybe"
            // is not a particularly useful scheduler feature
            if (active->cancellation_requested &&
                !active->cancellation_forced &&
                now - active->cancellation_requested_at >=
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

        // the process is gone, so both files are closed and stable now. stage
        // before publishing the terminal state; qstat should not say "done"
        // while its output is still stranded on an execution node
        if (auto staged = active->output_spool.stage(); !staged) {
            if (logger_ != nullptr) {
                logger_->warning(
                    "executor",
                    "job " + std::to_string(active->job_id) +
                        " output staging failed, spool kept at " +
                        active->output_spool.directory().string() + ": " +
                        staged.error().message);
            }

            // this is expected to be recoverable once remote nodes exist. keep
            // the finished job, its allocation, and its spool around; the next
            // tick will retry instead of killing rlbsd over a flaky transfer
            ++active;
            continue;
        }

        const auto final_state =
            active->walltime_exceeded
                ? JobState::failed
                : active->cancellation_requested ? JobState::cancelled
                                                 : JobState::completed;
        auto completed = repository_.transition(
            active->job_id, {
                                .state = final_state,
                                .assigned_node = std::nullopt,
                                .result = **result,
                                .detail =
                                    active->walltime_exceeded
                                        ? "walltime exceeded"
                                    : active->cancellation_requested
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
                    << (active->walltime_exceeded
                            ? "failed walltime_exceeded"
                        : active->cancellation_requested ? "cancelled"
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

    auto runtime_environment =
        PreparedRuntimeEnvironment::create(job, assignment.node_id);

    if (!runtime_environment) {
        const auto runtime_message = runtime_environment.error().message;
        auto failed = repository_.transition(
            job.id,
            {
                .state = JobState::failed,
                .assigned_node = std::nullopt,
                .result = std::nullopt,
                .detail = "could not prepare pbs runtime: " + runtime_message,
            });
        static_cast<void>(release(assignment.allocation));

        if (!failed) {
            return std::unexpected{repository_failure(
                LocalCoordinatorOperation::prepare_runtime_environment,
                std::move(failed.error()))};
        }

        if (logger_ != nullptr) {
            logger_->warning("executor",
                             "job " + std::to_string(job.id) +
                                 " runtime setup failed: " + runtime_message);
        }

        return std::unexpected{runtime_environment_failure(
            std::move(runtime_environment.error()))};
    }

    auto output_spool = PreparedOutputSpool::create(spool_directory_, job);

    if (!output_spool) {
        const auto spool_message = output_spool.error().message;
        auto failed = repository_.transition(
            job.id,
            {
                .state = JobState::failed,
                .assigned_node = std::nullopt,
                .result = std::nullopt,
                .detail = "could not prepare output spool: " + spool_message,
            });
        static_cast<void>(release(assignment.allocation));

        if (!failed) {
            return std::unexpected{repository_failure(
                LocalCoordinatorOperation::prepare_output_spool,
                std::move(failed.error()))};
        }

        if (logger_ != nullptr) {
            logger_->warning("executor",
                             "job " + std::to_string(job.id) +
                                 " spool setup failed: " + spool_message);
        }

        return std::unexpected{output_spool_failure(
            LocalCoordinatorOperation::prepare_output_spool,
            std::move(output_spool.error()))};
    }

    auto launched =
        runner_.launch(process_spec(job, *runtime_environment, *output_spool));

    if (!launched) {
        // open/exec failures can still leave useful stderr in the spool. stage
        // it through the normal path instead of making launch errors special
        auto staged = output_spool->stage();
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
        if (!staged) {
            return std::unexpected{output_spool_failure(
                LocalCoordinatorOperation::stage_output,
                std::move(staged.error()))};
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
        auto staged = output_spool->stage();
        static_cast<void>(repository_.transition(
            job.id, {
                        .state = JobState::failed,
                        .assigned_node = std::nullopt,
                        .result = std::nullopt,
                        .detail = "could not store the running state",
                    }));
        static_cast<void>(release(assignment.allocation));

        if (!staged) {
            return std::unexpected{output_spool_failure(
                LocalCoordinatorOperation::stage_output,
                std::move(staged.error()))};
        }

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
        .runtime_environment = std::move(*runtime_environment),
        .output_spool = std::move(*output_spool),
        .walltime = job.spec.walltime,
        .started_at = std::chrono::steady_clock::now(),
        .cancellation_requested = false,
        .cancellation_forced = false,
        .walltime_exceeded = false,
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
        .runtime_environment_error = std::nullopt,
        .output_spool_error = std::nullopt,
    }};
}

} // namespace rlbs
