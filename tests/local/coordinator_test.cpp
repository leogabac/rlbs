#include <rlbs/local/coordinator.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
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
            (std::filesystem::temp_directory_path() / "rlbs-local-XXXXXX")
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

[[nodiscard]] std::string read_file(const std::filesystem::path& path) {
    std::ifstream input{path};
    return {
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{},
    };
}

[[nodiscard]] rlbs::JobSpec
local_spec(const std::filesystem::path& working_directory,
           std::vector<std::string> argv) {
    return {
        .name = "local test",
        .resources =
            {
                .cpus = 2,
                .memory_mb = 1024,
                .gpus = 0,
            },
        .argv = std::move(argv),
        .working_directory = working_directory,
        .environment = {},
        .inherit_environment = true,
        .stdout_path = "job.out",
        .stderr_path = "job.err",
        .append_output = false,
    };
}

[[nodiscard]] rlbs::Node local_node() {
    return {
        "local",
        {
            .cpus = 2,
            .memory_mb = 4096,
            .gpus = 0,
        },
    };
}

[[nodiscard]] bool run_until_idle(rlbs::LocalCoordinator& coordinator) {
    constexpr int attempts = 200;

    for (int attempt = 0; attempt < attempts; ++attempt) {
        const auto ticked = coordinator.tick();

        if (!ticked) {
            return false;
        }

        if (coordinator.active_job_count() == 0) {
            return true;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }

    return false;
}

void test_queued_job_runs_to_completion() {
    TemporaryDirectory temporary;
    auto database =
        rlbs::SqliteDatabase::open(temporary.path() / "completed.db");

    expect(database.has_value(), "completion database opens");

    if (!database) {
        return;
    }

    rlbs::JobRepository repository{*database};
    const auto submitted = repository.submit(local_spec(
        temporary.path(), {"/bin/sh", "-c", "printf 'scheduled\\n'; exit 7"}));

    expect(submitted.has_value(), "local job submits");

    if (!submitted) {
        return;
    }

    rlbs::FirstFitScheduler scheduler;
    rlbs::LocalCoordinator coordinator{repository, local_node(), scheduler};

    const auto first_tick = coordinator.tick();
    expect(first_tick.has_value(), "first coordinator tick succeeds");
    expect(coordinator.active_job_count() == 1, "scheduled job becomes active");
    expect(coordinator.local_node().available().cpus == 0,
           "running job holds its cpu allocation");
    expect(run_until_idle(coordinator), "local job eventually finishes");

    const auto loaded = repository.find(submitted->id);
    expect(loaded && *loaded, "completed local job reloads");

    if (loaded && *loaded) {
        expect((*loaded)->state == rlbs::JobState::completed,
               "local job reaches completed");
        expect((*loaded)->assigned_node == "local",
               "local job keeps its assigned node");
        expect((*loaded)->result && (*loaded)->result->exit_code == 7,
               "local job stores its real exit code");
    }

    expect(read_file(temporary.path() / "job.out") == "scheduled\n",
           "local job writes its configured output");
    expect(coordinator.local_node().available().cpus == 2,
           "finished job returns its cpu allocation");

    const auto events = repository.events(submitted->id);
    expect(events && events->size() == 4,
           "local lifecycle stores four transitions");
}

void test_job_waits_when_resources_do_not_fit() {
    TemporaryDirectory temporary;
    auto database = rlbs::SqliteDatabase::open(temporary.path() / "waiting.db");

    expect(database.has_value(), "waiting database opens");

    if (!database) {
        return;
    }

    rlbs::JobRepository repository{*database};
    auto spec = local_spec(temporary.path(), {"/bin/true"});
    spec.resources.cpus = 3;
    const auto submitted = repository.submit(spec);

    if (!submitted) {
        expect(false, "waiting job submits");
        return;
    }

    rlbs::FirstFitScheduler scheduler;
    rlbs::LocalCoordinator coordinator{repository, local_node(), scheduler};

    expect(coordinator.tick().has_value(), "waiting tick succeeds");
    expect(coordinator.active_job_count() == 0,
           "oversized job does not launch");

    const auto loaded = repository.find(submitted->id);
    expect(loaded && *loaded && (*loaded)->state == rlbs::JobState::pending,
           "oversized job stays pending");
    expect(coordinator.local_node().available().cpus == 2,
           "waiting job consumes no resources");
}

void test_launch_failure_marks_job_failed() {
    TemporaryDirectory temporary;
    auto database =
        rlbs::SqliteDatabase::open(temporary.path() / "launch-failure.db");

    expect(database.has_value(), "launch failure database opens");

    if (!database) {
        return;
    }

    rlbs::JobRepository repository{*database};
    const auto submitted = repository.submit(
        local_spec(temporary.path(), {"/definitely/not/an/rlbs/executable"}));

    if (!submitted) {
        expect(false, "launch failure job submits");
        return;
    }

    rlbs::FirstFitScheduler scheduler;
    rlbs::LocalCoordinator coordinator{repository, local_node(), scheduler};

    expect(coordinator.tick().has_value(),
           "bad executable fails the job without breaking the tick");
    expect(coordinator.active_job_count() == 0,
           "failed launch leaves no active process");

    const auto loaded = repository.find(submitted->id);
    expect(loaded && *loaded && (*loaded)->state == rlbs::JobState::failed,
           "launch failure is persisted");
    expect(coordinator.local_node().available().cpus == 2,
           "launch failure returns its allocation");

    const auto events = repository.events(submitted->id);
    expect(events && events->size() == 3,
           "launch failure records assignment, starting, and failure");

    if (events && events->size() == 3) {
        expect((*events)[2].state == rlbs::JobState::failed,
               "launch failure event uses failed state");
    }
}

} // namespace

int main() {
    test_queued_job_runs_to_completion();
    test_job_waits_when_resources_do_not_fit();
    test_launch_failure_marks_job_failed();

    if (failures == 0) {
        std::cout << "all local coordinator tests passed\n";
    }

    return failures == 0 ? 0 : 1;
}
