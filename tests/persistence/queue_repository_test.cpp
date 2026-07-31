#include <rlbs/persistence/database.hpp>
#include <rlbs/persistence/queue_repository.hpp>

#include <filesystem>
#include <iostream>
#include <string_view>
#include <vector>

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
            (std::filesystem::temp_directory_path() / "rlbs-queues-XXXXXX")
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

void test_default_queue_is_seeded() {
    TemporaryDirectory temporary;
    auto database =
        rlbs::SqliteDatabase::open(temporary.path() / "default.db");

    expect(database.has_value(), "default queue database opens");

    if (!database) {
        return;
    }

    rlbs::QueueRepository repository{*database};
    const auto queue = repository.find("default");

    expect(queue && queue->has_value(), "migration creates the default queue");

    if (queue && *queue) {
        expect((*queue)->priority == 0, "default queue has neutral priority");
        expect((*queue)->enabled, "default queue accepts submissions");
        expect((*queue)->started, "default queue dispatches jobs");
        expect(!(*queue)->max_running,
               "default queue has no running-job limit");
    }
}

void test_add_find_and_reopen() {
    TemporaryDirectory temporary;
    const auto path = temporary.path() / "round-trip.db";

    {
        auto database = rlbs::SqliteDatabase::open(path);
        expect(database.has_value(), "queue database opens");

        if (!database) {
            return;
        }

        rlbs::QueueRepository repository{*database};
        const auto added = repository.add({
            .name = "short",
            .priority = 100,
            .enabled = true,
            .started = false,
            .max_running = 4,
        });

        expect(added.has_value(), "queue can be added");
    }

    auto database = rlbs::SqliteDatabase::open(path);
    expect(database.has_value(), "queue database reopens");

    if (!database) {
        return;
    }

    rlbs::QueueRepository repository{*database};
    const auto loaded = repository.find("short");

    expect(loaded && loaded->has_value(), "added queue survives reopening");

    if (loaded && *loaded) {
        expect((*loaded)->priority == 100, "queue priority survives");
        expect((*loaded)->enabled, "queue enabled state survives");
        expect(!(*loaded)->started, "queue started state survives");
        expect((*loaded)->max_running == 4,
               "queue running limit survives");
    }

    const auto missing = repository.find("missing");
    expect(missing && !*missing, "missing queue returns an empty result");
}

void test_lists_priority_then_name() {
    TemporaryDirectory temporary;
    auto database = rlbs::SqliteDatabase::open(temporary.path() / "list.db");

    expect(database.has_value(), "queue list database opens");

    if (!database) {
        return;
    }

    rlbs::QueueRepository repository{*database};
    const auto low = repository.add({
        .name = "low",
        .priority = -10,
        .enabled = true,
        .started = true,
        .max_running = std::nullopt,
    });
    const auto zebra = repository.add({
        .name = "zebra",
        .priority = 20,
        .enabled = true,
        .started = true,
        .max_running = std::nullopt,
    });
    const auto alpha = repository.add({
        .name = "alpha",
        .priority = 20,
        .enabled = true,
        .started = true,
        .max_running = std::nullopt,
    });

    expect(low && zebra && alpha, "queue list fixtures are added");

    const auto queues = repository.all();
    expect(queues && queues->size() == 4,
           "queue list includes custom and default queues");

    if (queues && queues->size() == 4) {
        expect((*queues)[0].name == "alpha",
               "same-priority queues sort by name");
        expect((*queues)[1].name == "zebra",
               "higher-priority queues come first");
        expect((*queues)[2].name == "default",
               "default queue keeps its neutral priority");
        expect((*queues)[3].name == "low",
               "lower-priority queues come last");
    }
}

void test_rejects_invalid_or_duplicate_queues() {
    TemporaryDirectory temporary;
    auto database =
        rlbs::SqliteDatabase::open(temporary.path() / "invalid.db");

    expect(database.has_value(), "invalid queue database opens");

    if (!database) {
        return;
    }

    rlbs::QueueRepository repository{*database};
    const auto blank = repository.add({
        .name = "   ",
        .priority = 0,
        .enabled = true,
        .started = true,
        .max_running = std::nullopt,
    });
    const auto duplicate = repository.add({
        .name = "default",
        .priority = 0,
        .enabled = true,
        .started = true,
        .max_running = std::nullopt,
    });
    const auto zero_limit =
        repository.add({.name = "zero", .max_running = 0});

    expect(!blank, "blank queue name is rejected");
    expect(!duplicate, "duplicate queue name is rejected");
    expect(!zero_limit, "zero running limit is rejected");
}

void test_updates_queue_switches() {
    TemporaryDirectory temporary;
    auto database =
        rlbs::SqliteDatabase::open(temporary.path() / "switches.db");

    expect(database.has_value(), "queue switches database opens");

    if (!database) {
        return;
    }

    rlbs::QueueRepository repository{*database};
    const auto stopped = repository.set_started("default", false);
    const auto disabled = repository.set_enabled("default", false);

    expect(stopped && !stopped->started, "queue can be stopped");
    expect(disabled && !disabled->enabled, "queue can be disabled");

    const auto restarted = repository.set_started("default", true);
    const auto enabled = repository.set_enabled("default", true);
    expect(restarted && restarted->started, "queue can be restarted");
    expect(enabled && enabled->enabled, "queue can be enabled again");

    expect(!repository.set_started("missing", false),
           "updating a missing queue is rejected");
}

} // namespace

int main() {
    test_default_queue_is_seeded();
    test_add_find_and_reopen();
    test_lists_priority_then_name();
    test_rejects_invalid_or_duplicate_queues();
    test_updates_queue_switches();

    if (failures == 0) {
        std::cout << "all queue repository tests passed\n";
    }

    return failures == 0 ? 0 : 1;
}
