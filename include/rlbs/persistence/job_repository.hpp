#pragma once

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
    choose_queue_sequence,
    insert_job,
    insert_argument,
    insert_environment,
    commit_transaction,
    read_job,
    read_arguments,
    read_environment,
};

struct RepositoryError {
    RepositoryOperation operation{RepositoryOperation::read_job};
    int sqlite_code{0};
    std::string message;
};

// the database still owns the connection; this just gives job-shaped methods
// to the rest of rlbs so sqlite details do not crawl into scheduler code
class JobRepository {
  public:
    explicit JobRepository(SqliteDatabase& database);

    [[nodiscard]] std::expected<Job, RepositoryError>
    submit(const JobSpec& spec);

    [[nodiscard]] std::expected<std::optional<Job>, RepositoryError>
    find(JobId id) const;

    [[nodiscard]] std::expected<std::vector<Job>, RepositoryError>
    pending() const;

  private:
    sqlite3* connection_{nullptr};
};

} // namespace rlbs
