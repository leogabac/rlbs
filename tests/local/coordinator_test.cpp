#include <rlbs/local/coordinator.hpp>

#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
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
        .walltime = std::nullopt,
        .queue = "default",
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
    constexpr int attempts = 800;

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
    rlbs::QueueRepository queues{*database};
    const auto submitted = repository.submit(local_spec(
        temporary.path(), {"/bin/sh", "-c", "printf 'scheduled\\n'; exit 7"}));

    expect(submitted.has_value(), "local job submits");

    if (!submitted) {
        return;
    }

    rlbs::FirstFitScheduler scheduler;
    rlbs::LocalCoordinator coordinator{
        repository, queues, local_node(), scheduler,
        temporary.path() / "spool"};

    const auto first_tick = coordinator.tick();
    expect(first_tick.has_value(), "first coordinator tick succeeds");
    expect(coordinator.active_job_count() == 1, "scheduled job becomes active");
    expect(coordinator.local_node().available().cpus == 0,
           "running job holds its cpu allocation");
    expect(!std::filesystem::exists(temporary.path() / "job.out"),
           "final stdout stays absent until the job is staged");
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
    rlbs::QueueRepository queues{*database};
    auto spec = local_spec(temporary.path(), {"/bin/true"});
    spec.resources.cpus = 3;
    const auto submitted = repository.submit(spec);

    if (!submitted) {
        expect(false, "waiting job submits");
        return;
    }

    rlbs::FirstFitScheduler scheduler;
    rlbs::LocalCoordinator coordinator{
        repository, queues, local_node(), scheduler,
        temporary.path() / "spool"};

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
    rlbs::QueueRepository queues{*database};
    const auto submitted = repository.submit(
        local_spec(temporary.path(), {"/definitely/not/an/rlbs/executable"}));

    if (!submitted) {
        expect(false, "launch failure job submits");
        return;
    }

    rlbs::FirstFitScheduler scheduler;
    rlbs::LocalCoordinator coordinator{
        repository, queues, local_node(), scheduler,
        temporary.path() / "spool"};

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

void test_pending_job_can_be_cancelled() {
    TemporaryDirectory temporary;
    auto database =
        rlbs::SqliteDatabase::open(temporary.path() / "cancel-pending.db");

    expect(database.has_value(), "pending cancellation database opens");

    if (!database) {
        return;
    }

    rlbs::JobRepository repository{*database};
    rlbs::QueueRepository queues{*database};
    const auto submitted = repository.submit(
        local_spec(temporary.path(), {"/bin/sh", "-c", "sleep 30"}));

    if (!submitted) {
        expect(false, "pending cancellation job submits");
        return;
    }

    rlbs::FirstFitScheduler scheduler;
    rlbs::LocalCoordinator coordinator{
        repository, queues, local_node(), scheduler,
        temporary.path() / "spool"};

    expect(coordinator.cancel(submitted->id).has_value(),
           "pending job cancellation succeeds");
    expect(coordinator.tick().has_value(),
           "cancelled pending job does not break the next tick");
    expect(coordinator.active_job_count() == 0,
           "cancelled pending job never launches");

    const auto loaded = repository.find(submitted->id);
    expect(loaded && *loaded && (*loaded)->state == rlbs::JobState::cancelled,
           "pending cancellation is persisted");
    expect(!coordinator.cancel(submitted->id),
           "terminal job cannot be cancelled again");
}

void test_running_job_can_be_cancelled() {
    TemporaryDirectory temporary;
    auto database =
        rlbs::SqliteDatabase::open(temporary.path() / "cancel-running.db");

    expect(database.has_value(), "running cancellation database opens");

    if (!database) {
        return;
    }

    rlbs::JobRepository repository{*database};
    rlbs::QueueRepository queues{*database};
    const auto submitted = repository.submit(
        local_spec(temporary.path(), {"/bin/sh", "-c", "sleep 30"}));

    if (!submitted) {
        expect(false, "running cancellation job submits");
        return;
    }

    rlbs::FirstFitScheduler scheduler;
    rlbs::LocalCoordinator coordinator{
        repository, queues, local_node(), scheduler,
        temporary.path() / "spool"};

    expect(coordinator.tick().has_value(), "cancellation job starts");
    expect(coordinator.active_job_count() == 1,
           "cancellation fixture becomes active");
    expect(coordinator.cancel(submitted->id).has_value(),
           "running job accepts cancellation");
    expect(coordinator.cancel(submitted->id).has_value(),
           "repeated running cancellation is harmless");
    expect(run_until_idle(coordinator),
           "cancelled process is eventually reaped");

    const auto loaded = repository.find(submitted->id);
    expect(loaded && *loaded && (*loaded)->state == rlbs::JobState::cancelled,
           "running cancellation is persisted");
    expect(loaded && *loaded && (*loaded)->result &&
               (*loaded)->result->terminating_signal == SIGTERM,
           "running cancellation stores the terminating signal");
    expect(coordinator.local_node().available().cpus == 2,
           "cancelled job returns its cpu allocation");
}

void test_pbs_runtime_environment() {
    TemporaryDirectory temporary;
    auto database =
        rlbs::SqliteDatabase::open(temporary.path() / "pbs-runtime.db");

    expect(database.has_value(), "pbs runtime database opens");

    if (!database) {
        return;
    }

    rlbs::JobRepository repository{*database};
    rlbs::QueueRepository queues{*database};
    auto spec = local_spec(
        temporary.path(),
        {"/bin/sh", "-c",
         "printf '%s\\n' \"$PBS_JOBID\" \"$PBS_JOBNAME\" \"$PBS_O_WORKDIR\" "
         "\"$PBS_NODEFILE\"; cat \"$PBS_NODEFILE\""});
    spec.name = "pbs runtime";
    spec.inherit_environment = false;
    spec.stdout_path = "pbs-runtime.out";
    spec.environment = {
        {.name = "PBS_JOBID", .value = "fake-id"},
        {.name = "PBS_JOBNAME", .value = "fake-name"},
        {.name = "PBS_O_WORKDIR", .value = "/fake/work"},
        {.name = "PBS_NODEFILE", .value = "/fake/nodes"},
    };
    const auto submitted = repository.submit(spec);

    if (!submitted) {
        expect(false, "pbs runtime job submits");
        return;
    }

    rlbs::FirstFitScheduler scheduler;
    rlbs::LocalCoordinator coordinator{
        repository, queues, local_node(), scheduler,
        temporary.path() / "spool"};

    expect(coordinator.tick().has_value(), "pbs runtime job starts");
    expect(run_until_idle(coordinator), "pbs runtime job finishes");

    std::istringstream output{read_file(temporary.path() / "pbs-runtime.out")};
    std::vector<std::string> lines;
    std::string line;

    while (std::getline(output, line)) {
        lines.push_back(std::move(line));
    }

    expect(lines.size() == 6, "pbs runtime output has every expected line");

    if (lines.size() == 6) {
        expect(lines[0] == std::to_string(submitted->id),
               "pbs job id comes from the scheduler");
        expect(lines[1] == "pbs runtime",
               "pbs job name comes from the scheduler");
        expect(lines[2] == temporary.path().string(),
               "pbs original work directory is the submitted directory");
        expect(lines[4] == "local" && lines[5] == "local",
               "pbs node file repeats the node once per requested cpu");
        expect(!std::filesystem::exists(lines[3]),
               "pbs node file is removed after the job exits");
    }
}

void test_walltime_stops_running_job() {
    TemporaryDirectory temporary;
    auto database =
        rlbs::SqliteDatabase::open(temporary.path() / "walltime.db");

    expect(database.has_value(), "walltime database opens");

    if (!database) {
        return;
    }

    rlbs::JobRepository repository{*database};
    rlbs::QueueRepository queues{*database};
    auto spec = local_spec(
        temporary.path(),
        {"/bin/sh", "-c", "printf 'before timeout\\n'; sleep 30"});
    spec.walltime = std::chrono::seconds{1};
    const auto submitted = repository.submit(spec);

    if (!submitted) {
        expect(false, "walltime job submits");
        return;
    }

    rlbs::FirstFitScheduler scheduler;
    rlbs::LocalCoordinator coordinator{
        repository, queues, local_node(), scheduler,
        temporary.path() / "spool"};

    expect(coordinator.tick().has_value(), "walltime job starts");
    expect(run_until_idle(coordinator), "walltime job is eventually stopped");

    const auto loaded = repository.find(submitted->id);
    expect(loaded && *loaded && (*loaded)->state == rlbs::JobState::failed,
           "walltime expiry marks the job failed");
    expect(loaded && *loaded && (*loaded)->result &&
               (*loaded)->result->terminating_signal == SIGTERM,
           "walltime expiry stores the terminating signal");
    expect(loaded && *loaded && (*loaded)->execution_time &&
               *(*loaded)->execution_time >= std::chrono::seconds{1},
           "walltime job stores its execution time");
    expect(read_file(temporary.path() / "job.out") == "before timeout\n",
           "walltime staging preserves output written before termination");
    expect(coordinator.local_node().available().cpus == 2,
           "walltime job returns its cpu allocation");
}

void test_coordinator_enforces_queue_running_limit() {
    TemporaryDirectory temporary;
    auto database =
        rlbs::SqliteDatabase::open(temporary.path() / "queue-policy.db");

    expect(database.has_value(), "queue policy database opens");

    if (!database) {
        return;
    }

    rlbs::QueueRepository queues{*database};
    const auto limited_queue = queues.add({
        .name = "limited",
        .priority = 100,
        .enabled = true,
        .started = true,
        .max_running = 1,
    });
    const auto other_queue = queues.add({
        .name = "other",
        .priority = 10,
        .enabled = true,
        .started = true,
        .max_running = std::nullopt,
    });
    expect(limited_queue && other_queue, "queue policy fixtures are added");

    if (!limited_queue || !other_queue) {
        return;
    }

    rlbs::JobRepository repository{*database};
    auto first_spec = local_spec(
        temporary.path(), {"/bin/sh", "-c", "sleep 0.2"});
    first_spec.name = "limited first";
    first_spec.resources.cpus = 1;
    first_spec.stdout_path = "limited-first.out";
    first_spec.stderr_path = "limited-first.err";
    first_spec.queue = "limited";

    auto second_spec =
        local_spec(temporary.path(), {"/bin/sh", "-c", "printf second"});
    second_spec.name = "limited second";
    second_spec.resources.cpus = 1;
    second_spec.stdout_path = "limited-second.out";
    second_spec.stderr_path = "limited-second.err";
    second_spec.queue = "limited";

    auto other_spec =
        local_spec(temporary.path(), {"/bin/sh", "-c", "printf other"});
    other_spec.name = "other";
    other_spec.resources.cpus = 1;
    other_spec.stdout_path = "other.out";
    other_spec.stderr_path = "other.err";
    other_spec.queue = "other";

    const auto first = repository.submit(first_spec);
    const auto second = repository.submit(second_spec);
    const auto other = repository.submit(other_spec);
    expect(first && second && other, "queue policy jobs submit");

    if (!first || !second || !other) {
        return;
    }

    rlbs::FirstFitScheduler scheduler;
    rlbs::LocalCoordinator coordinator{
        repository, queues, local_node(), scheduler,
        temporary.path() / "spool"};

    expect(coordinator.tick().has_value(),
           "first limited queue job starts");
    expect(coordinator.tick().has_value(),
           "another queue gets the next scheduling turn");

    const auto first_loaded = repository.find(first->id);
    const auto second_loaded = repository.find(second->id);
    const auto other_loaded = repository.find(other->id);
    expect(first_loaded && *first_loaded &&
               (*first_loaded)->state == rlbs::JobState::running,
           "first limited job remains active");
    expect(second_loaded && *second_loaded &&
               (*second_loaded)->state == rlbs::JobState::pending,
           "second limited job waits at max_running");
    expect(other_loaded && *other_loaded &&
               (*other_loaded)->state == rlbs::JobState::running,
           "lower-priority queue runs while limited queue is full");

    expect(run_until_idle(coordinator),
           "queue policy jobs eventually finish");

    const auto second_finished = repository.find(second->id);
    expect(second_finished && *second_finished &&
               (*second_finished)->state == rlbs::JobState::completed,
           "waiting limited job starts after its queue slot opens");
}

} // namespace

int main() {
    test_queued_job_runs_to_completion();
    test_job_waits_when_resources_do_not_fit();
    test_launch_failure_marks_job_failed();
    test_pending_job_can_be_cancelled();
    test_running_job_can_be_cancelled();
    test_pbs_runtime_environment();
    test_walltime_stops_running_job();
    test_coordinator_enforces_queue_running_limit();

    if (failures == 0) {
        std::cout << "all local coordinator tests passed\n";
    }

    return failures == 0 ? 0 : 1;
}
