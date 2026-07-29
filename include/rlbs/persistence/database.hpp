#pragma once

#include <expected>
#include <filesystem>
#include <string>

struct sqlite3;

namespace rlbs {

enum class DatabaseOperation {
    open,
    configure,
    read_schema_version,
    migrate,
};

struct DatabaseError {
    DatabaseOperation operation{DatabaseOperation::open};
    int sqlite_code{0};
    std::string message;
};

// this owns one sqlite connection. keeping the raw handle stuck in here means
// the repository layer cannot accidentally turn into sqlite calls everywhere
class SqliteDatabase {
  public:
    [[nodiscard]] static std::expected<SqliteDatabase, DatabaseError>
    open(const std::filesystem::path& path);

    SqliteDatabase(const SqliteDatabase&) = delete;
    SqliteDatabase& operator=(const SqliteDatabase&) = delete;
    SqliteDatabase(SqliteDatabase&& other) noexcept;
    SqliteDatabase& operator=(SqliteDatabase&&) = delete;
    ~SqliteDatabase();

    [[nodiscard]] std::expected<int, DatabaseError> schema_version() const;
    [[nodiscard]] std::expected<bool, DatabaseError>
    foreign_keys_enabled() const;

  private:
    explicit SqliteDatabase(sqlite3* connection);

    [[nodiscard]] std::expected<void, DatabaseError> configure();
    [[nodiscard]] std::expected<void, DatabaseError> migrate();
    [[nodiscard]] std::expected<void, DatabaseError>
    execute(const char* sql, DatabaseOperation operation);
    [[nodiscard]] std::expected<int, DatabaseError>
    query_integer(const char* sql, DatabaseOperation operation) const;

    sqlite3* connection_{nullptr};
};

} // namespace rlbs
