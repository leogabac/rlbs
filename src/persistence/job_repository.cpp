#include <rlbs/persistence/job_repository.hpp>

#include <cstdint>
#include <limits>
#include <string_view>
#include <utility>

#include <sqlite3.h>

namespace rlbs {
namespace {

// statements are another sqlite thing that must be cleaned up on every exit.
// letting one leak can even make closing the database fail, which is fun
class UniqueStatement {
  public:
    explicit UniqueStatement(sqlite3_stmt* statement) : statement_{statement} {}

    UniqueStatement(const UniqueStatement&) = delete;
    UniqueStatement& operator=(const UniqueStatement&) = delete;

    UniqueStatement(UniqueStatement&& other) noexcept
        : statement_{std::exchange(other.statement_, nullptr)} {}

    UniqueStatement& operator=(UniqueStatement&&) = delete;

    ~UniqueStatement() {
        if (statement_ != nullptr) {
            static_cast<void>(sqlite3_finalize(statement_));
        }
    }

    [[nodiscard]] sqlite3_stmt* get() const { return statement_; }

  private:
    sqlite3_stmt* statement_{nullptr};
};

[[nodiscard]] RepositoryError error(sqlite3* connection,
                                    RepositoryOperation operation,
                                    int sqlite_code, std::string message = {}) {
    if (message.empty()) {
        message = sqlite3_errmsg(connection);
    }

    return {
        .operation = operation,
        .sqlite_code = sqlite_code,
        .message = std::move(message),
    };
}

[[nodiscard]] std::expected<UniqueStatement, RepositoryError>
prepare(sqlite3* connection, const char* sql, RepositoryOperation operation) {
    sqlite3_stmt* statement = nullptr;
    const int result =
        sqlite3_prepare_v2(connection, sql, -1, &statement, nullptr);

    if (result != SQLITE_OK) {
        return std::unexpected{error(connection, operation, result)};
    }

    return UniqueStatement{statement};
}

[[nodiscard]] std::expected<void, RepositoryError>
execute(sqlite3* connection, const char* sql, RepositoryOperation operation) {
    char* sqlite_message = nullptr;
    const int result =
        sqlite3_exec(connection, sql, nullptr, nullptr, &sqlite_message);

    if (result == SQLITE_OK) {
        return {};
    }

    std::string message =
        sqlite_message != nullptr ? sqlite_message : sqlite3_errmsg(connection);
    sqlite3_free(sqlite_message);
    return std::unexpected{
        error(connection, operation, result, std::move(message))};
}

void rollback(sqlite3* connection) {
    // the original error is the useful one, so rollback gets a best-effort
    // cleanup instead of replacing it with a second complaint
    static_cast<void>(
        sqlite3_exec(connection, "ROLLBACK;", nullptr, nullptr, nullptr));
}

[[nodiscard]] std::expected<void, RepositoryError>
bind_integer(sqlite3* connection, sqlite3_stmt* statement, int position,
             std::int64_t value, RepositoryOperation operation) {
    const int result = sqlite3_bind_int64(statement, position, value);

    if (result != SQLITE_OK) {
        return std::unexpected{error(connection, operation, result)};
    }

    return {};
}

[[nodiscard]] std::expected<void, RepositoryError>
bind_text(sqlite3* connection, sqlite3_stmt* statement, int position,
          std::string_view value, RepositoryOperation operation) {
    const int result =
        sqlite3_bind_text64(statement, position, value.data(), value.size(),
                            SQLITE_TRANSIENT, SQLITE_UTF8);

    if (result != SQLITE_OK) {
        return std::unexpected{error(connection, operation, result)};
    }

    return {};
}

[[nodiscard]] std::expected<void, RepositoryError>
bind_optional_path(sqlite3* connection, sqlite3_stmt* statement, int position,
                   const std::optional<std::filesystem::path>& path,
                   RepositoryOperation operation) {
    if (!path) {
        const int result = sqlite3_bind_null(statement, position);

        if (result != SQLITE_OK) {
            return std::unexpected{error(connection, operation, result)};
        }

        return {};
    }

    return bind_text(connection, statement, position, path->string(),
                     operation);
}

[[nodiscard]] std::expected<void, RepositoryError>
step_done(sqlite3* connection, sqlite3_stmt* statement,
          RepositoryOperation operation) {
    const int result = sqlite3_step(statement);

    if (result != SQLITE_DONE) {
        return std::unexpected{error(connection, operation, result)};
    }

    return {};
}

[[nodiscard]] std::string column_text(sqlite3_stmt* statement, int column) {
    const auto* value = sqlite3_column_text(statement, column);
    const auto size = sqlite3_column_bytes(statement, column);

    return {reinterpret_cast<const char*>(value),
            static_cast<std::size_t>(size)};
}

[[nodiscard]] std::optional<std::string>
optional_column_text(sqlite3_stmt* statement, int column) {
    if (sqlite3_column_type(statement, column) == SQLITE_NULL) {
        return std::nullopt;
    }

    return column_text(statement, column);
}

[[nodiscard]] std::optional<int>
optional_column_integer(sqlite3_stmt* statement, int column) {
    if (sqlite3_column_type(statement, column) == SQLITE_NULL) {
        return std::nullopt;
    }

    return sqlite3_column_int(statement, column);
}

[[nodiscard]] std::optional<JobState> decode_state(std::string_view state) {
    if (state == "pending") {
        return JobState::pending;
    }
    if (state == "assigned") {
        return JobState::assigned;
    }
    if (state == "starting") {
        return JobState::starting;
    }
    if (state == "running") {
        return JobState::running;
    }
    if (state == "completed") {
        return JobState::completed;
    }
    if (state == "failed") {
        return JobState::failed;
    }
    if (state == "cancelled") {
        return JobState::cancelled;
    }

    return std::nullopt;
}

[[nodiscard]] std::expected<std::uint64_t, RepositoryError>
next_queue_sequence(sqlite3* connection) {
    auto statement = prepare(
        connection, "SELECT COALESCE(MAX(queue_sequence), 0) + 1 FROM jobs;",
        RepositoryOperation::choose_queue_sequence);

    if (!statement) {
        return std::unexpected{std::move(statement.error())};
    }

    const int result = sqlite3_step(statement->get());

    if (result != SQLITE_ROW) {
        return std::unexpected{error(
            connection, RepositoryOperation::choose_queue_sequence, result)};
    }

    const auto value = sqlite3_column_int64(statement->get(), 0);

    if (value <= 0) {
        return std::unexpected{
            error(connection, RepositoryOperation::choose_queue_sequence,
                  SQLITE_RANGE, "queue sequence ran out of usable values")};
    }

    return static_cast<std::uint64_t>(value);
}

[[nodiscard]] std::expected<void, RepositoryError>
insert_job_row(sqlite3* connection, const JobSpec& spec,
               std::uint64_t queue_sequence) {
    constexpr const char* sql = R"sql(
INSERT INTO jobs (
    queue_sequence,
    name,
    state,
    cpus,
    memory_mb,
    gpus,
    working_directory,
    inherit_environment,
    stdout_path,
    stderr_path,
    append_output
) VALUES (?, ?, 'pending', ?, ?, ?, ?, ?, ?, ?, ?);
)sql";
    auto statement = prepare(connection, sql, RepositoryOperation::insert_job);

    if (!statement) {
        return std::unexpected{std::move(statement.error())};
    }

    const auto memory_limit =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());

    if (spec.resources.memory_mb > memory_limit) {
        return std::unexpected{
            error(connection, RepositoryOperation::insert_job, SQLITE_RANGE,
                  "memory request does not fit in sqlite integer storage")};
    }

    auto* raw = statement->get();
    const auto operation = RepositoryOperation::insert_job;

    if (auto result =
            bind_integer(connection, raw, 1,
                         static_cast<std::int64_t>(queue_sequence), operation);
        !result) {
        return result;
    }
    if (auto result = bind_text(connection, raw, 2, spec.name, operation);
        !result) {
        return result;
    }
    if (auto result =
            bind_integer(connection, raw, 3, spec.resources.cpus, operation);
        !result) {
        return result;
    }
    if (auto result = bind_integer(
            connection, raw, 4,
            static_cast<std::int64_t>(spec.resources.memory_mb), operation);
        !result) {
        return result;
    }
    if (auto result =
            bind_integer(connection, raw, 5, spec.resources.gpus, operation);
        !result) {
        return result;
    }
    if (auto result = bind_text(connection, raw, 6,
                                spec.working_directory.string(), operation);
        !result) {
        return result;
    }
    if (auto result = bind_integer(connection, raw, 7,
                                   spec.inherit_environment ? 1 : 0, operation);
        !result) {
        return result;
    }
    if (auto result =
            bind_optional_path(connection, raw, 8, spec.stdout_path, operation);
        !result) {
        return result;
    }
    if (auto result =
            bind_optional_path(connection, raw, 9, spec.stderr_path, operation);
        !result) {
        return result;
    }
    if (auto result = bind_integer(connection, raw, 10,
                                   spec.append_output ? 1 : 0, operation);
        !result) {
        return result;
    }

    return step_done(connection, raw, operation);
}

[[nodiscard]] std::expected<void, RepositoryError>
insert_arguments(sqlite3* connection, JobId job_id,
                 const std::vector<std::string>& arguments) {
    auto statement = prepare(
        connection,
        "INSERT INTO job_arguments (job_id, position, value) VALUES (?, ?, ?);",
        RepositoryOperation::insert_argument);

    if (!statement) {
        return std::unexpected{std::move(statement.error())};
    }

    auto* raw = statement->get();

    for (std::size_t position = 0; position < arguments.size(); ++position) {
        static_cast<void>(sqlite3_reset(raw));
        static_cast<void>(sqlite3_clear_bindings(raw));

        if (auto result = bind_integer(connection, raw, 1,
                                       static_cast<std::int64_t>(job_id),
                                       RepositoryOperation::insert_argument);
            !result) {
            return result;
        }
        if (auto result = bind_integer(connection, raw, 2,
                                       static_cast<std::int64_t>(position),
                                       RepositoryOperation::insert_argument);
            !result) {
            return result;
        }
        if (auto result = bind_text(connection, raw, 3, arguments[position],
                                    RepositoryOperation::insert_argument);
            !result) {
            return result;
        }
        if (auto result = step_done(connection, raw,
                                    RepositoryOperation::insert_argument);
            !result) {
            return result;
        }
    }

    return {};
}

[[nodiscard]] std::expected<void, RepositoryError>
insert_environment(sqlite3* connection, JobId job_id,
                   const std::vector<EnvironmentVariable>& environment) {
    auto statement = prepare(
        connection,
        "INSERT INTO job_environment (job_id, name, value) VALUES (?, ?, ?);",
        RepositoryOperation::insert_environment);

    if (!statement) {
        return std::unexpected{std::move(statement.error())};
    }

    auto* raw = statement->get();

    for (const auto& variable : environment) {
        static_cast<void>(sqlite3_reset(raw));
        static_cast<void>(sqlite3_clear_bindings(raw));

        if (auto result = bind_integer(connection, raw, 1,
                                       static_cast<std::int64_t>(job_id),
                                       RepositoryOperation::insert_environment);
            !result) {
            return result;
        }
        if (auto result = bind_text(connection, raw, 2, variable.name,
                                    RepositoryOperation::insert_environment);
            !result) {
            return result;
        }
        if (auto result = bind_text(connection, raw, 3, variable.value,
                                    RepositoryOperation::insert_environment);
            !result) {
            return result;
        }
        if (auto result = step_done(connection, raw,
                                    RepositoryOperation::insert_environment);
            !result) {
            return result;
        }
    }

    return {};
}

[[nodiscard]] std::expected<void, RepositoryError>
load_arguments(sqlite3* connection, Job& job) {
    auto statement = prepare(
        connection,
        "SELECT value FROM job_arguments WHERE job_id = ? ORDER BY position;",
        RepositoryOperation::read_arguments);

    if (!statement) {
        return std::unexpected{std::move(statement.error())};
    }

    if (auto bound = bind_integer(connection, statement->get(), 1,
                                  static_cast<std::int64_t>(job.id),
                                  RepositoryOperation::read_arguments);
        !bound) {
        return bound;
    }

    int result = SQLITE_ROW;

    while ((result = sqlite3_step(statement->get())) == SQLITE_ROW) {
        job.spec.argv.push_back(column_text(statement->get(), 0));
    }

    if (result != SQLITE_DONE) {
        return std::unexpected{
            error(connection, RepositoryOperation::read_arguments, result)};
    }

    return {};
}

[[nodiscard]] std::expected<void, RepositoryError>
load_environment(sqlite3* connection, Job& job) {
    auto statement = prepare(connection,
                             "SELECT name, value FROM job_environment "
                             "WHERE job_id = ? ORDER BY name;",
                             RepositoryOperation::read_environment);

    if (!statement) {
        return std::unexpected{std::move(statement.error())};
    }

    if (auto bound = bind_integer(connection, statement->get(), 1,
                                  static_cast<std::int64_t>(job.id),
                                  RepositoryOperation::read_environment);
        !bound) {
        return bound;
    }

    int result = SQLITE_ROW;

    while ((result = sqlite3_step(statement->get())) == SQLITE_ROW) {
        job.spec.environment.push_back({
            .name = column_text(statement->get(), 0),
            .value = column_text(statement->get(), 1),
        });
    }

    if (result != SQLITE_DONE) {
        return std::unexpected{
            error(connection, RepositoryOperation::read_environment, result)};
    }

    return {};
}

} // namespace

JobRepository::JobRepository(SqliteDatabase& database)
    : connection_{database.connection_} {}

std::expected<Job, RepositoryError> JobRepository::submit(const JobSpec& spec) {
    if (auto begun = execute(connection_, "BEGIN IMMEDIATE;",
                             RepositoryOperation::begin_transaction);
        !begun) {
        return std::unexpected{std::move(begun.error())};
    }

    auto queue_sequence = next_queue_sequence(connection_);

    if (!queue_sequence) {
        rollback(connection_);
        return std::unexpected{std::move(queue_sequence.error())};
    }

    if (auto inserted = insert_job_row(connection_, spec, *queue_sequence);
        !inserted) {
        rollback(connection_);
        return std::unexpected{std::move(inserted.error())};
    }

    const auto job_id =
        static_cast<JobId>(sqlite3_last_insert_rowid(connection_));

    if (auto inserted = insert_arguments(connection_, job_id, spec.argv);
        !inserted) {
        rollback(connection_);
        return std::unexpected{std::move(inserted.error())};
    }

    if (auto inserted =
            insert_environment(connection_, job_id, spec.environment);
        !inserted) {
        rollback(connection_);
        return std::unexpected{std::move(inserted.error())};
    }

    if (auto committed = execute(connection_, "COMMIT;",
                                 RepositoryOperation::commit_transaction);
        !committed) {
        rollback(connection_);
        return std::unexpected{std::move(committed.error())};
    }

    return Job{
        .id = job_id,
        .queue_sequence = *queue_sequence,
        .spec = spec,
        .state = JobState::pending,
        .assigned_node = std::nullopt,
        .result = std::nullopt,
    };
}

std::expected<std::optional<Job>, RepositoryError>
JobRepository::find(JobId id) const {
    if (id > static_cast<JobId>(std::numeric_limits<std::int64_t>::max())) {
        return std::optional<Job>{};
    }

    constexpr const char* sql = R"sql(
SELECT
    id,
    queue_sequence,
    name,
    state,
    cpus,
    memory_mb,
    gpus,
    working_directory,
    inherit_environment,
    stdout_path,
    stderr_path,
    append_output,
    assigned_node,
    exit_code,
    terminating_signal,
    dumped_core
FROM jobs
WHERE id = ?;
)sql";
    auto statement = prepare(connection_, sql, RepositoryOperation::read_job);

    if (!statement) {
        return std::unexpected{std::move(statement.error())};
    }

    if (auto bound = bind_integer(connection_, statement->get(), 1,
                                  static_cast<std::int64_t>(id),
                                  RepositoryOperation::read_job);
        !bound) {
        return std::unexpected{std::move(bound.error())};
    }

    const int result = sqlite3_step(statement->get());

    if (result == SQLITE_DONE) {
        return std::optional<Job>{};
    }

    if (result != SQLITE_ROW) {
        return std::unexpected{
            error(connection_, RepositoryOperation::read_job, result)};
    }

    const auto state = decode_state(column_text(statement->get(), 3));

    if (!state) {
        return std::unexpected{error(connection_, RepositoryOperation::read_job,
                                     SQLITE_CORRUPT,
                                     "job contains an unknown state")};
    }

    Job job{
        .id = static_cast<JobId>(sqlite3_column_int64(statement->get(), 0)),
        .queue_sequence = static_cast<std::uint64_t>(
            sqlite3_column_int64(statement->get(), 1)),
        .spec =
            {
                .name = column_text(statement->get(), 2),
                .resources =
                    {
                        .cpus = static_cast<std::uint32_t>(
                            sqlite3_column_int64(statement->get(), 4)),
                        .memory_mb = static_cast<std::uint64_t>(
                            sqlite3_column_int64(statement->get(), 5)),
                        .gpus = static_cast<std::uint32_t>(
                            sqlite3_column_int64(statement->get(), 6)),
                    },
                .argv = {},
                .working_directory = column_text(statement->get(), 7),
                .environment = {},
                .inherit_environment =
                    sqlite3_column_int(statement->get(), 8) != 0,
                .stdout_path = optional_column_text(statement->get(), 9)
                                   .transform([](std::string value) {
                                       return std::filesystem::path{
                                           std::move(value)};
                                   }),
                .stderr_path = optional_column_text(statement->get(), 10)
                                   .transform([](std::string value) {
                                       return std::filesystem::path{
                                           std::move(value)};
                                   }),
                .append_output = sqlite3_column_int(statement->get(), 11) != 0,
            },
        .state = *state,
        .assigned_node = optional_column_text(statement->get(), 12),
        .result = std::nullopt,
    };

    const auto exit_code = optional_column_integer(statement->get(), 13);
    const auto terminating_signal =
        optional_column_integer(statement->get(), 14);
    const bool dumped_core = sqlite3_column_int(statement->get(), 15) != 0;

    if (exit_code || terminating_signal || dumped_core) {
        job.result = JobResult{
            .exit_code = exit_code,
            .terminating_signal = terminating_signal,
            .dumped_core = dumped_core,
        };
    }

    if (auto loaded = load_arguments(connection_, job); !loaded) {
        return std::unexpected{std::move(loaded.error())};
    }

    if (auto loaded = load_environment(connection_, job); !loaded) {
        return std::unexpected{std::move(loaded.error())};
    }

    return std::optional<Job>{std::move(job)};
}

std::expected<std::vector<Job>, RepositoryError>
JobRepository::pending() const {
    auto statement = prepare(connection_,
                             "SELECT id FROM jobs WHERE state = 'pending' "
                             "ORDER BY queue_sequence, id;",
                             RepositoryOperation::read_job);

    if (!statement) {
        return std::unexpected{std::move(statement.error())};
    }

    std::vector<JobId> ids;
    int result = SQLITE_ROW;

    while ((result = sqlite3_step(statement->get())) == SQLITE_ROW) {
        ids.push_back(
            static_cast<JobId>(sqlite3_column_int64(statement->get(), 0)));
    }

    if (result != SQLITE_DONE) {
        return std::unexpected{
            error(connection_, RepositoryOperation::read_job, result)};
    }

    std::vector<Job> jobs;
    jobs.reserve(ids.size());

    for (const auto id : ids) {
        auto job = find(id);

        if (!job) {
            return std::unexpected{std::move(job.error())};
        }

        // ids came from the same connection a few lines ago, so disappearing
        // here would mean somebody bypassed the repository and got creative
        if (!*job) {
            return std::unexpected{
                error(connection_, RepositoryOperation::read_job,
                      SQLITE_CORRUPT, "pending job disappeared while loading")};
        }

        jobs.push_back(std::move(**job));
    }

    return jobs;
}

} // namespace rlbs
