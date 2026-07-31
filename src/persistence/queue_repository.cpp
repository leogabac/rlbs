// queue changes are intentionally tiny atomic updates. scheduling rereads them
// every tick, so stop really means "do not launch the next one" without a
// daemon restart or some second in-memory copy getting stale.
#include <rlbs/persistence/queue_repository.hpp>

#include <cstdint>
#include <limits>
#include <utility>

#include <sqlite3.h>

namespace rlbs {
namespace {

// sqlite makes callers clean up prepared statements manually. keep that mess
// inside a tiny owner so every early return still does the boring right thing
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

[[nodiscard]] QueueRepositoryError
error(sqlite3* connection, QueueRepositoryOperation operation, int sqlite_code,
      std::string message = {}) {
    if (message.empty()) {
        message = sqlite3_errmsg(connection);
    }

    return {
        .operation = operation,
        .sqlite_code = sqlite_code,
        .message = std::move(message),
    };
}

[[nodiscard]] std::expected<UniqueStatement, QueueRepositoryError>
prepare(sqlite3* connection, const char* sql,
        QueueRepositoryOperation operation) {
    sqlite3_stmt* statement = nullptr;
    const int result =
        sqlite3_prepare_v2(connection, sql, -1, &statement, nullptr);

    if (result != SQLITE_OK) {
        return std::unexpected{error(connection, operation, result)};
    }

    return UniqueStatement{statement};
}

[[nodiscard]] std::expected<void, QueueRepositoryError>
bind_text(sqlite3* connection, sqlite3_stmt* statement, int position,
          std::string_view value, QueueRepositoryOperation operation) {
    const int result =
        sqlite3_bind_text64(statement, position, value.data(), value.size(),
                            SQLITE_TRANSIENT, SQLITE_UTF8);

    if (result != SQLITE_OK) {
        return std::unexpected{error(connection, operation, result)};
    }

    return {};
}

[[nodiscard]] std::expected<void, QueueRepositoryError>
bind_integer(sqlite3* connection, sqlite3_stmt* statement, int position,
             std::int64_t value, QueueRepositoryOperation operation) {
    const int result = sqlite3_bind_int64(statement, position, value);

    if (result != SQLITE_OK) {
        return std::unexpected{error(connection, operation, result)};
    }

    return {};
}

[[nodiscard]] std::expected<void, QueueRepositoryError>
bind_optional_integer(sqlite3* connection, sqlite3_stmt* statement,
                      int position,
                      const std::optional<std::uint32_t>& value,
                      QueueRepositoryOperation operation) {
    if (!value) {
        const int result = sqlite3_bind_null(statement, position);

        if (result != SQLITE_OK) {
            return std::unexpected{error(connection, operation, result)};
        }

        return {};
    }

    return bind_integer(connection, statement, position, *value, operation);
}

[[nodiscard]] std::string column_text(sqlite3_stmt* statement, int column) {
    const auto* value = sqlite3_column_text(statement, column);
    const auto size = sqlite3_column_bytes(statement, column);

    return {reinterpret_cast<const char*>(value),
            static_cast<std::size_t>(size)};
}

[[nodiscard]] std::expected<BatchQueue, QueueRepositoryError>
read_queue(sqlite3* connection, sqlite3_stmt* statement,
           QueueRepositoryOperation operation) {
    std::optional<std::uint32_t> max_running;

    if (sqlite3_column_type(statement, 4) != SQLITE_NULL) {
        const auto stored = sqlite3_column_int64(statement, 4);

        // repository-created rows cannot hit this, but a manually edited db
        // should fail loudly instead of wrapping into a nonsense queue limit
        if (stored <= 0 ||
            stored > std::numeric_limits<std::uint32_t>::max()) {
            return std::unexpected{
                error(connection, operation, SQLITE_MISMATCH,
                      "queue max_running is outside the supported range")};
        }

        max_running = static_cast<std::uint32_t>(stored);
    }

    return BatchQueue{
        .name = column_text(statement, 0),
        .priority = sqlite3_column_int(statement, 1),
        .enabled = sqlite3_column_int(statement, 2) != 0,
        .started = sqlite3_column_int(statement, 3) != 0,
        .max_running = max_running,
    };
}

} // namespace

QueueRepository::QueueRepository(SqliteDatabase& database)
    : connection_{database.connection_} {}

std::expected<BatchQueue, QueueRepositoryError>
QueueRepository::add(const BatchQueue& queue) {
    constexpr const char* sql = R"sql(
INSERT INTO queues (name, priority, enabled, started, max_running)
VALUES (?, ?, ?, ?, ?);
)sql";
    const auto operation = QueueRepositoryOperation::insert_queue;
    auto statement = prepare(connection_, sql, operation);

    if (!statement) {
        return std::unexpected{std::move(statement.error())};
    }

    auto* raw = statement->get();

    if (auto result = bind_text(connection_, raw, 1, queue.name, operation);
        !result) {
        return std::unexpected{std::move(result.error())};
    }
    if (auto result =
            bind_integer(connection_, raw, 2, queue.priority, operation);
        !result) {
        return std::unexpected{std::move(result.error())};
    }
    if (auto result =
            bind_integer(connection_, raw, 3, queue.enabled, operation);
        !result) {
        return std::unexpected{std::move(result.error())};
    }
    if (auto result =
            bind_integer(connection_, raw, 4, queue.started, operation);
        !result) {
        return std::unexpected{std::move(result.error())};
    }
    if (auto result = bind_optional_integer(connection_, raw, 5,
                                            queue.max_running, operation);
        !result) {
        return std::unexpected{std::move(result.error())};
    }

    const int result = sqlite3_step(raw);

    if (result != SQLITE_DONE) {
        return std::unexpected{error(connection_, operation, result)};
    }

    return queue;
}

std::expected<std::optional<BatchQueue>, QueueRepositoryError>
QueueRepository::find(std::string_view name) const {
    constexpr const char* sql = R"sql(
SELECT name, priority, enabled, started, max_running
FROM queues
WHERE name = ?;
)sql";
    const auto operation = QueueRepositoryOperation::read_queue;
    auto statement = prepare(connection_, sql, operation);

    if (!statement) {
        return std::unexpected{std::move(statement.error())};
    }

    if (auto result =
            bind_text(connection_, statement->get(), 1, name, operation);
        !result) {
        return std::unexpected{std::move(result.error())};
    }

    const int result = sqlite3_step(statement->get());

    if (result == SQLITE_DONE) {
        return std::optional<BatchQueue>{};
    }
    if (result != SQLITE_ROW) {
        return std::unexpected{error(connection_, operation, result)};
    }

    auto queue = read_queue(connection_, statement->get(), operation);

    if (!queue) {
        return std::unexpected{std::move(queue.error())};
    }

    return std::optional<BatchQueue>{std::move(*queue)};
}

std::expected<std::vector<BatchQueue>, QueueRepositoryError>
QueueRepository::all() const {
    constexpr const char* sql = R"sql(
SELECT name, priority, enabled, started, max_running
FROM queues
ORDER BY priority DESC, name ASC;
)sql";
    const auto operation = QueueRepositoryOperation::list_queues;
    auto statement = prepare(connection_, sql, operation);

    if (!statement) {
        return std::unexpected{std::move(statement.error())};
    }

    std::vector<BatchQueue> queues;

    while (true) {
        const int result = sqlite3_step(statement->get());

        if (result == SQLITE_DONE) {
            return queues;
        }
        if (result != SQLITE_ROW) {
            return std::unexpected{error(connection_, operation, result)};
        }

        auto queue = read_queue(connection_, statement->get(), operation);

        if (!queue) {
            return std::unexpected{std::move(queue.error())};
        }

        queues.push_back(std::move(*queue));
    }
}

std::expected<BatchQueue, QueueRepositoryError>
QueueRepository::set_started(std::string_view name, bool started) {
    constexpr const char* sql = "UPDATE queues SET started = ? WHERE name = ?;";
    const auto operation = QueueRepositoryOperation::update_queue;
    auto statement = prepare(connection_, sql, operation);

    if (!statement) {
        return std::unexpected{std::move(statement.error())};
    }

    if (auto bound =
            bind_integer(connection_, statement->get(), 1, started, operation);
        !bound) {
        return std::unexpected{std::move(bound.error())};
    }
    if (auto bound =
            bind_text(connection_, statement->get(), 2, name, operation);
        !bound) {
        return std::unexpected{std::move(bound.error())};
    }

    const int result = sqlite3_step(statement->get());

    if (result != SQLITE_DONE) {
        return std::unexpected{error(connection_, operation, result)};
    }

    auto updated = find(name);

    if (!updated) {
        return std::unexpected{QueueRepositoryError{
            .operation = operation,
            .sqlite_code = updated.error().sqlite_code,
            .message = std::move(updated.error().message),
        }};
    }
    if (!*updated) {
        return std::unexpected{
            error(connection_, operation, SQLITE_NOTFOUND,
                  "queue does not exist: " + std::string{name})};
    }

    return std::move(**updated);
}

std::expected<BatchQueue, QueueRepositoryError>
QueueRepository::set_enabled(std::string_view name, bool enabled) {
    constexpr const char* sql = "UPDATE queues SET enabled = ? WHERE name = ?;";
    const auto operation = QueueRepositoryOperation::update_queue;
    auto statement = prepare(connection_, sql, operation);

    if (!statement) {
        return std::unexpected{std::move(statement.error())};
    }

    if (auto bound =
            bind_integer(connection_, statement->get(), 1, enabled, operation);
        !bound) {
        return std::unexpected{std::move(bound.error())};
    }
    if (auto bound =
            bind_text(connection_, statement->get(), 2, name, operation);
        !bound) {
        return std::unexpected{std::move(bound.error())};
    }

    const int result = sqlite3_step(statement->get());

    if (result != SQLITE_DONE) {
        return std::unexpected{error(connection_, operation, result)};
    }

    auto updated = find(name);

    if (!updated) {
        return std::unexpected{QueueRepositoryError{
            .operation = operation,
            .sqlite_code = updated.error().sqlite_code,
            .message = std::move(updated.error().message),
        }};
    }
    if (!*updated) {
        return std::unexpected{
            error(connection_, operation, SQLITE_NOTFOUND,
                  "queue does not exist: " + std::string{name})};
    }

    return std::move(**updated);
}

} // namespace rlbs
