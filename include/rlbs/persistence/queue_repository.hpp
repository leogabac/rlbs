// queue policy has to survive daemon restarts, so this is the one place that
// turns queue-shaped requests into sqlite instead of leaking sql into control
// handlers and scheduler code.
#pragma once

#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rlbs/core/batch_queue.hpp>
#include <rlbs/persistence/database.hpp>

struct sqlite3;

namespace rlbs {

enum class QueueRepositoryOperation {
    insert_queue,
    update_queue,
    read_queue,
    list_queues,
};

struct QueueRepositoryError {
    QueueRepositoryOperation operation{QueueRepositoryOperation::read_queue};
    int sqlite_code{0};
    std::string message;
};

// queues are scheduler policy stored in the same database as jobs. this keeps
// them alive across daemon restarts without making them their own little world
class QueueRepository {
  public:
    explicit QueueRepository(SqliteDatabase& database);

    [[nodiscard]] std::expected<BatchQueue, QueueRepositoryError>
    add(const BatchQueue& queue);

    [[nodiscard]]
    std::expected<std::optional<BatchQueue>, QueueRepositoryError>
    find(std::string_view name) const;

    [[nodiscard]] std::expected<std::vector<BatchQueue>, QueueRepositoryError>
    all() const;

    [[nodiscard]] std::expected<BatchQueue, QueueRepositoryError>
    set_started(std::string_view name, bool started);

    [[nodiscard]] std::expected<BatchQueue, QueueRepositoryError>
    set_enabled(std::string_view name, bool enabled);

  private:
    sqlite3* connection_{nullptr};
};

} // namespace rlbs
