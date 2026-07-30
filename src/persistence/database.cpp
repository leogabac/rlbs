#include <rlbs/persistence/database.hpp>

#include <utility>

#include <sqlite3.h>

namespace rlbs {
namespace {

constexpr int current_schema_version = 2;

// sqlite statements need finalizing on every return path, including the
// annoying ones. this little owner keeps that cleanup local instead of trusting
// memory
class UniqueStatement {
  public:
    explicit UniqueStatement(sqlite3_stmt* statement) : statement_{statement} {}

    UniqueStatement(const UniqueStatement&) = delete;
    UniqueStatement& operator=(const UniqueStatement&) = delete;
    UniqueStatement(UniqueStatement&&) = delete;
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

[[nodiscard]] DatabaseError error(sqlite3* connection,
                                  DatabaseOperation operation, int sqlite_code,
                                  std::string message = {}) {
    if (message.empty() && connection != nullptr) {
        message = sqlite3_errmsg(connection);
    }

    return {
        .operation = operation,
        .sqlite_code = sqlite_code,
        .message = std::move(message),
    };
}

constexpr const char* schema_v1 = R"sql(
CREATE TABLE jobs (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    queue_sequence INTEGER NOT NULL UNIQUE,
    name TEXT NOT NULL,
    state TEXT NOT NULL CHECK (
        state IN (
            'pending',
            'assigned',
            'starting',
            'running',
            'completed',
            'failed',
            'cancelled'
        )
    ),
    cpus INTEGER NOT NULL CHECK (cpus > 0),
    memory_mb INTEGER NOT NULL CHECK (memory_mb >= 0),
    gpus INTEGER NOT NULL CHECK (gpus >= 0),
    working_directory TEXT NOT NULL,
    inherit_environment INTEGER NOT NULL CHECK (
        inherit_environment IN (0, 1)
    ),
    stdout_path TEXT,
    stderr_path TEXT,
    append_output INTEGER NOT NULL CHECK (append_output IN (0, 1)),
    assigned_node TEXT,
    exit_code INTEGER,
    terminating_signal INTEGER,
    dumped_core INTEGER NOT NULL DEFAULT 0 CHECK (dumped_core IN (0, 1)),
    submitted_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    CHECK (exit_code IS NULL OR terminating_signal IS NULL)
);

CREATE TABLE job_arguments (
    job_id INTEGER NOT NULL REFERENCES jobs(id) ON DELETE CASCADE,
    position INTEGER NOT NULL CHECK (position >= 0),
    value TEXT NOT NULL,
    PRIMARY KEY (job_id, position)
);

CREATE TABLE job_environment (
    job_id INTEGER NOT NULL REFERENCES jobs(id) ON DELETE CASCADE,
    name TEXT NOT NULL CHECK (length(name) > 0),
    value TEXT NOT NULL,
    PRIMARY KEY (job_id, name)
);

CREATE TABLE job_events (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    job_id INTEGER NOT NULL REFERENCES jobs(id) ON DELETE CASCADE,
    occurred_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    state TEXT NOT NULL CHECK (
        state IN (
            'pending',
            'assigned',
            'starting',
            'running',
            'completed',
            'failed',
            'cancelled'
        )
    ),
    detail TEXT
);

CREATE INDEX jobs_queue_index ON jobs(state, queue_sequence);
CREATE INDEX job_events_job_index ON job_events(job_id, id);

PRAGMA user_version = 1;
)sql";

constexpr const char* schema_v2 = R"sql(
ALTER TABLE jobs ADD COLUMN walltime_seconds INTEGER
    CHECK (walltime_seconds IS NULL OR walltime_seconds > 0);
ALTER TABLE jobs ADD COLUMN started_at INTEGER;
ALTER TABLE jobs ADD COLUMN finished_at INTEGER;

PRAGMA user_version = 2;
)sql";

} // namespace

SqliteDatabase::SqliteDatabase(sqlite3* connection) : connection_{connection} {}

SqliteDatabase::SqliteDatabase(SqliteDatabase&& other) noexcept
    : connection_{std::exchange(other.connection_, nullptr)} {}

SqliteDatabase::~SqliteDatabase() {
    if (connection_ != nullptr) {
        static_cast<void>(sqlite3_close(connection_));
    }
}

std::expected<SqliteDatabase, DatabaseError>
SqliteDatabase::open(const std::filesystem::path& path) {
    sqlite3* connection = nullptr;
    const int result = sqlite3_open_v2(
        path.c_str(), &connection,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
        nullptr);

    if (result != SQLITE_OK) {
        auto open_error = error(connection, DatabaseOperation::open, result);

        if (connection != nullptr) {
            static_cast<void>(sqlite3_close(connection));
        }

        return std::unexpected{std::move(open_error)};
    }

    SqliteDatabase database{connection};

    if (auto configured = database.configure(); !configured) {
        return std::unexpected{std::move(configured.error())};
    }

    if (auto migrated = database.migrate(); !migrated) {
        return std::unexpected{std::move(migrated.error())};
    }

    return database;
}

std::expected<void, DatabaseError> SqliteDatabase::configure() {
    static_cast<void>(sqlite3_extended_result_codes(connection_, 1));

    const int timeout_result = sqlite3_busy_timeout(connection_, 5000);

    if (timeout_result != SQLITE_OK) {
        return std::unexpected{
            error(connection_, DatabaseOperation::configure, timeout_result)};
    }

    // foreign keys are per connection in sqlite, because apparently enabling
    // them in the schema once would have been too convenient
    return execute("PRAGMA foreign_keys = ON;", DatabaseOperation::configure);
}

std::expected<void, DatabaseError> SqliteDatabase::migrate() {
    auto version = schema_version();

    if (!version) {
        return std::unexpected{std::move(version.error())};
    }

    if (*version > current_schema_version) {
        return std::unexpected{
            error(connection_, DatabaseOperation::migrate, SQLITE_ERROR,
                  "database schema is newer than this rlbs build")};
    }

    if (*version == current_schema_version) {
        return {};
    }

    if (auto begun = execute("BEGIN IMMEDIATE;", DatabaseOperation::migrate);
        !begun) {
        return begun;
    }

    if (*version < 1) {
        if (auto created = execute(schema_v1, DatabaseOperation::migrate);
            !created) {
            static_cast<void>(execute("ROLLBACK;", DatabaseOperation::migrate));
            return created;
        }
    }

    if (*version < 2) {
        if (auto upgraded = execute(schema_v2, DatabaseOperation::migrate);
            !upgraded) {
            static_cast<void>(execute("ROLLBACK;", DatabaseOperation::migrate));
            return upgraded;
        }
    }

    if (auto committed = execute("COMMIT;", DatabaseOperation::migrate);
        !committed) {
        static_cast<void>(execute("ROLLBACK;", DatabaseOperation::migrate));
        return committed;
    }

    return {};
}

std::expected<void, DatabaseError>
SqliteDatabase::execute(const char* sql, DatabaseOperation operation) {
    char* sqlite_message = nullptr;
    const int result =
        sqlite3_exec(connection_, sql, nullptr, nullptr, &sqlite_message);

    if (result == SQLITE_OK) {
        return {};
    }

    std::string message = sqlite_message != nullptr
                              ? sqlite_message
                              : sqlite3_errmsg(connection_);
    sqlite3_free(sqlite_message);
    return std::unexpected{
        error(connection_, operation, result, std::move(message))};
}

std::expected<int, DatabaseError>
SqliteDatabase::query_integer(const char* sql,
                              DatabaseOperation operation) const {
    sqlite3_stmt* raw_statement = nullptr;
    const int prepare_result =
        sqlite3_prepare_v2(connection_, sql, -1, &raw_statement, nullptr);

    if (prepare_result != SQLITE_OK) {
        return std::unexpected{error(connection_, operation, prepare_result)};
    }

    UniqueStatement statement{raw_statement};
    const int step_result = sqlite3_step(statement.get());

    if (step_result != SQLITE_ROW) {
        return std::unexpected{error(connection_, operation, step_result)};
    }

    return sqlite3_column_int(statement.get(), 0);
}

std::expected<int, DatabaseError> SqliteDatabase::schema_version() const {
    return query_integer("PRAGMA user_version;",
                         DatabaseOperation::read_schema_version);
}

std::expected<bool, DatabaseError>
SqliteDatabase::foreign_keys_enabled() const {
    auto enabled =
        query_integer("PRAGMA foreign_keys;", DatabaseOperation::configure);

    if (!enabled) {
        return std::unexpected{std::move(enabled.error())};
    }

    return *enabled != 0;
}

} // namespace rlbs
