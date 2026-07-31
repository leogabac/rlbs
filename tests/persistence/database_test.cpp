#include <rlbs/persistence/database.hpp>

#include <filesystem>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <sqlite3.h>
#include <unistd.h>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "failed: " << message << '\n';
        ++failures;
    }
}

class TemporaryDirectory {
  public:
    TemporaryDirectory() {
        auto pattern =
            (std::filesystem::temp_directory_path() / "rlbs-database-XXXXXX")
                .string();
        std::vector<char> writable_pattern(pattern.begin(), pattern.end());
        writable_pattern.push_back('\0');

        if (const auto* created = ::mkdtemp(writable_pattern.data())) {
            path_ = created;
        }
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    ~TemporaryDirectory() {
        if (!path_.empty()) {
            std::error_code ignored;
            std::filesystem::remove_all(path_, ignored);
        }
    }

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

  private:
    std::filesystem::path path_;
};

[[nodiscard]] std::set<std::string>
read_table_names(const std::filesystem::path& path) {
    sqlite3* connection = nullptr;
    std::set<std::string> names;

    if (sqlite3_open_v2(path.c_str(), &connection, SQLITE_OPEN_READONLY,
                        nullptr) != SQLITE_OK) {
        if (connection != nullptr) {
            static_cast<void>(sqlite3_close(connection));
        }

        return names;
    }

    sqlite3_stmt* statement = nullptr;
    constexpr const char* query =
        "SELECT name FROM sqlite_schema WHERE type = 'table';";

    if (sqlite3_prepare_v2(connection, query, -1, &statement, nullptr) ==
        SQLITE_OK) {
        while (sqlite3_step(statement) == SQLITE_ROW) {
            names.emplace(reinterpret_cast<const char*>(
                sqlite3_column_text(statement, 0)));
        }
    }

    static_cast<void>(sqlite3_finalize(statement));
    static_cast<void>(sqlite3_close(connection));
    return names;
}

void test_initializes_and_reopens_database() {
    TemporaryDirectory temporary;
    const auto path = temporary.path() / "rlbs.db";

    {
        auto database = rlbs::SqliteDatabase::open(path);

        expect(database.has_value(), "a new database opens");

        if (!database) {
            return;
        }

        const auto version = database->schema_version();
        const auto foreign_keys = database->foreign_keys_enabled();

        expect(version && *version == 5,
               "a new database uses schema version 5");
        expect(foreign_keys && *foreign_keys,
               "foreign keys are enabled on the rlbs connection");
    }

    const auto table_names = read_table_names(path);
    expect(table_names.contains("jobs"), "schema creates the jobs table");
    expect(table_names.contains("job_arguments"),
           "schema creates the argument table");
    expect(table_names.contains("job_environment"),
           "schema creates the environment table");
    expect(table_names.contains("job_events"),
           "schema creates the event table");
    expect(table_names.contains("queues"), "schema creates the queues table");

    auto reopened = rlbs::SqliteDatabase::open(path);
    expect(reopened.has_value(), "an existing database reopens");

    if (reopened) {
        const auto version = reopened->schema_version();
        expect(version && *version == 5,
               "reopening does not rerun or change the schema");
    }
}

void test_rejects_newer_schema() {
    TemporaryDirectory temporary;
    const auto path = temporary.path() / "future.db";
    sqlite3* connection = nullptr;

    expect(sqlite3_open(path.c_str(), &connection) == SQLITE_OK,
           "future database fixture opens");

    if (connection == nullptr) {
        return;
    }

    expect(sqlite3_exec(connection, "PRAGMA user_version = 6;", nullptr,
                        nullptr, nullptr) == SQLITE_OK,
           "future database fixture sets its version");
    static_cast<void>(sqlite3_close(connection));

    const auto database = rlbs::SqliteDatabase::open(path);
    expect(!database, "a newer schema is rejected");
    expect(!database &&
               database.error().operation == rlbs::DatabaseOperation::migrate,
           "newer schema failure identifies migration");
}

void test_upgrades_version_three_database() {
    TemporaryDirectory temporary;
    const auto path = temporary.path() / "version-three.db";
    sqlite3* connection = nullptr;

    expect(sqlite3_open(path.c_str(), &connection) == SQLITE_OK,
           "version three fixture opens");

    if (connection == nullptr) {
        return;
    }

    constexpr const char* legacy_schema = R"sql(
CREATE TABLE queues (name TEXT PRIMARY KEY);
INSERT INTO queues (name) VALUES ('default');
CREATE TABLE jobs (
    id INTEGER PRIMARY KEY,
    state TEXT NOT NULL,
    queue_sequence INTEGER NOT NULL
);
INSERT INTO jobs (id, state, queue_sequence) VALUES (1, 'pending', 1);
PRAGMA user_version = 3;
)sql";
    expect(sqlite3_exec(connection, legacy_schema, nullptr, nullptr, nullptr) ==
               SQLITE_OK,
           "version three fixture creates its legacy schema");
    static_cast<void>(sqlite3_close(connection));

    const auto database = rlbs::SqliteDatabase::open(path);
    expect(database.has_value(), "version three database upgrades");

    if (database) {
        const auto version = database->schema_version();
        expect(version && *version == 5,
               "version three database reaches schema version 5");
    }

    connection = nullptr;
    expect(sqlite3_open_v2(path.c_str(), &connection, SQLITE_OPEN_READONLY,
                           nullptr) == SQLITE_OK,
           "upgraded database opens for inspection");

    sqlite3_stmt* statement = nullptr;
    if (connection != nullptr &&
        sqlite3_prepare_v2(connection,
                           "SELECT queue_name, owner_uid, owner_gid "
                           "FROM jobs WHERE id = 1;",
                           -1,
                           &statement, nullptr) == SQLITE_OK &&
        sqlite3_step(statement) == SQLITE_ROW) {
        const std::string_view queue{reinterpret_cast<const char*>(
            sqlite3_column_text(statement, 0))};
        expect(queue == "default",
               "migration moves old jobs into the default queue");
        expect(sqlite3_column_type(statement, 1) == SQLITE_NULL &&
                   sqlite3_column_type(statement, 2) == SQLITE_NULL,
               "migration keeps old jobs explicitly unowned");
    } else {
        expect(false, "upgraded job queue can be inspected");
    }

    static_cast<void>(sqlite3_finalize(statement));
    static_cast<void>(sqlite3_close(connection));
}

} // namespace

int main() {
    test_initializes_and_reopens_database();
    test_rejects_newer_schema();
    test_upgrades_version_three_database();

    if (failures == 0) {
        std::cout << "all persistence tests passed\n";
    }

    return failures == 0 ? 0 : 1;
}
