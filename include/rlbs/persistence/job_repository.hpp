#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

#include <rlbs/core/job.hpp>
#include <rlbs/persistence/database.hpp>

struct sqlite3;

namespace rlbs {

enum class RepositoryOperation {
    begin_transaction,
    validate_queue,
    choose_queue_sequence,
    insert_job,
    insert_argument,
    insert_environment,
    commit_transaction,
    read_job,
    read_arguments,
    read_environment,
    validate_transition,
    update_job,
    insert_event,
    read_events,
};

struct RepositoryError {
    RepositoryOperation operation{RepositoryOperation::read_job};
    int sqlite_code{0};
    std::string message;
};

struct JobTransition {
    JobState state{JobState::pending};
    std::optional<NodeId> assigned_node;
    std::optional<JobResult> result;
    std::optional<std::string> detail;
};

struct JobEvent {
    std::uint64_t id{0};
    JobId job_id{0};
    std::string occurred_at;
    JobState state{JobState::pending};
    std::optional<std::string> detail;
};

// the database still owns the connection; this just gives job-shaped methods
// to the rest of rlbs so sqlite details do not crawl into scheduler code
class JobRepository {
  public:
    explicit JobRepository(SqliteDatabase& database);

    [[nodiscard]] std::expected<Job, RepositoryError>
    submit(const JobSpec& spec, JobOwner owner);

    [[nodiscard]] std::expected<std::optional<Job>, RepositoryError>
    find(JobId id) const;

    [[nodiscard]] std::expected<std::vector<Job>, RepositoryError>
    pending() const;

    [[nodiscard]] std::expected<std::vector<Job>, RepositoryError>
    schedulable() const;

    // call this once before a fresh daemon starts scheduling. live records
    // cannot survive a daemon crash honestly: their process handles died with
    // the old daemon, so leaving them running would reserve the node forever.
    [[nodiscard]] std::expected<std::vector<Job>, RepositoryError>
    recover_interrupted_jobs();

    [[nodiscard]] std::expected<std::vector<Job>, RepositoryError> all() const;

    [[nodiscard]] std::expected<Job, RepositoryError>
    transition(JobId id, const JobTransition& update);

    [[nodiscard]] std::expected<std::vector<JobEvent>, RepositoryError>
    events(JobId id) const;

  private:
    sqlite3* connection_{nullptr};
};

} // namespace rlbs
