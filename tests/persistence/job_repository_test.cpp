#include <rlbs/persistence/database.hpp>
#include <rlbs/persistence/job_repository.hpp>
#include <rlbs/persistence/queue_repository.hpp>

#include <filesystem>
#include <iostream>
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
            (std::filesystem::temp_directory_path() / "rlbs-repository-XXXXXX")
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

[[nodiscard]] rlbs::JobSpec example_spec(std::string name) {
    return {
        .name = std::move(name),
        .resources =
            {
                .cpus = 4,
                .memory_mb = 8192,
                .gpus = 1,
            },
        .argv = {"/bin/sh", "-c", "printf hello"},
        .working_directory = "/tmp/work",
        .environment =
            {
                {.name = "FIRST", .value = "one"},
                {.name = "SECOND", .value = "two"},
            },
        .inherit_environment = false,
        .stdout_path = "job.out",
        .stderr_path = std::nullopt,
        .append_output = true,
        .walltime = std::chrono::seconds{90},
        .queue = "default",
    };
}

void expect_same_spec(const rlbs::JobSpec& actual,
                      const rlbs::JobSpec& expected) {
    expect(actual.name == expected.name, "job name survives persistence");
    expect(actual.resources.cpus == expected.resources.cpus,
           "cpu request survives persistence");
    expect(actual.resources.memory_mb == expected.resources.memory_mb,
           "memory request survives persistence");
    expect(actual.resources.gpus == expected.resources.gpus,
           "gpu request survives persistence");
    expect(actual.argv == expected.argv, "arguments survive persistence");
    expect(actual.working_directory == expected.working_directory,
           "working directory survives persistence");
    expect(actual.environment.size() == expected.environment.size(),
           "environment size survives persistence");

    if (actual.environment.size() == expected.environment.size()) {
        for (std::size_t index = 0; index < actual.environment.size();
             ++index) {
            expect(actual.environment[index].name ==
                       expected.environment[index].name,
                   "environment name survives persistence");
            expect(actual.environment[index].value ==
                       expected.environment[index].value,
                   "environment value survives persistence");
        }
    }

    expect(actual.inherit_environment == expected.inherit_environment,
           "environment inheritance survives persistence");
    expect(actual.stdout_path == expected.stdout_path,
           "stdout path survives persistence");
    expect(actual.stderr_path == expected.stderr_path,
           "stderr path survives persistence");
    expect(actual.append_output == expected.append_output,
           "output mode survives persistence");
    expect(actual.walltime == expected.walltime,
           "walltime survives persistence");
    expect(actual.queue == expected.queue, "queue survives persistence");
}

void test_submit_find_and_reopen() {
    TemporaryDirectory temporary;
    const auto path = temporary.path() / "rlbs.db";
    auto spec = example_spec("round trip");
    spec.queue = "short";
    rlbs::JobId submitted_id = 0;

    {
        auto database = rlbs::SqliteDatabase::open(path);
        expect(database.has_value(), "repository database opens");

        if (!database) {
            return;
        }

        rlbs::QueueRepository queues{*database};
        const auto added = queues.add({
            .name = "short",
            .priority = 100,
            .enabled = true,
            .started = true,
            .max_running = std::nullopt,
        });
        expect(added.has_value(), "job queue fixture is added");

        rlbs::JobRepository repository{*database};
        const auto submitted = repository.submit(spec);

        expect(submitted.has_value(), "job submission succeeds");

        if (!submitted) {
            return;
        }

        submitted_id = submitted->id;
        expect(submitted->id > 0, "submission assigns a job id");
        expect(submitted->queue_sequence == 1,
               "first submission gets the first queue position");
        expect(submitted->state == rlbs::JobState::pending,
               "new jobs start pending");
        expect_same_spec(submitted->spec, spec);
    }

    auto database = rlbs::SqliteDatabase::open(path);
    expect(database.has_value(), "repository database reopens");

    if (!database) {
        return;
    }

    rlbs::JobRepository repository{*database};
    const auto loaded = repository.find(submitted_id);

    expect(loaded && loaded->has_value(),
           "submitted job loads after reopening");

    if (loaded && *loaded) {
        expect((*loaded)->id == submitted_id, "loaded job keeps its id");
        expect((*loaded)->queue_sequence == 1,
               "loaded job keeps its queue position");
        expect((*loaded)->state == rlbs::JobState::pending,
               "loaded job keeps its state");
        expect_same_spec((*loaded)->spec, spec);
    }

    const auto missing = repository.find(submitted_id + 1000);
    expect(missing && !*missing, "missing job returns an empty result");
}

void test_pending_jobs_keep_fifo_order() {
    TemporaryDirectory temporary;
    auto database = rlbs::SqliteDatabase::open(temporary.path() / "fifo.db");

    expect(database.has_value(), "fifo database opens");

    if (!database) {
        return;
    }

    rlbs::JobRepository repository{*database};
    const auto first = repository.submit(example_spec("first"));
    const auto second = repository.submit(example_spec("second"));
    const auto third = repository.submit(example_spec("third"));

    expect(first && second && third, "fifo jobs submit");

    const auto pending = repository.pending();
    expect(pending.has_value(), "pending jobs load");

    if (pending) {
        expect(pending->size() == 3, "all pending jobs are returned");

        if (pending->size() == 3) {
            expect((*pending)[0].spec.name == "first",
                   "first submitted job stays first");
            expect((*pending)[1].spec.name == "second",
                   "second submitted job stays second");
            expect((*pending)[2].spec.name == "third",
                   "third submitted job stays third");
        }
    }
}

void test_all_jobs_include_finished_jobs() {
    TemporaryDirectory temporary;
    auto database = rlbs::SqliteDatabase::open(temporary.path() / "all.db");

    expect(database.has_value(), "all-jobs database opens");

    if (!database) {
        return;
    }

    rlbs::JobRepository repository{*database};
    const auto first = repository.submit(example_spec("finished"));
    const auto second = repository.submit(example_spec("waiting"));

    expect(first && second, "all-jobs fixtures submit");

    if (!first || !second) {
        return;
    }

    static_cast<void>(repository.transition(
        first->id, {
                       .state = rlbs::JobState::cancelled,
                       .assigned_node = std::nullopt,
                       .result = std::nullopt,
                       .detail = "cancelled in repository test",
                   }));

    const auto jobs = repository.all();
    expect(jobs && jobs->size() == 2,
           "all jobs includes terminal and pending jobs");
    const auto schedulable = repository.schedulable();
    expect(schedulable && schedulable->size() == 1,
           "scheduler query leaves finished history behind");

    if (jobs && jobs->size() == 2) {
        expect((*jobs)[0].state == rlbs::JobState::cancelled,
               "all jobs keeps the finished job");
        expect((*jobs)[1].state == rlbs::JobState::pending,
               "all jobs keeps the waiting job");
    }
    if (schedulable && schedulable->size() == 1) {
        expect((*schedulable)[0].state == rlbs::JobState::pending,
               "scheduler query keeps the waiting job");
    }
}

void test_failed_submission_rolls_back() {
    TemporaryDirectory temporary;
    auto database =
        rlbs::SqliteDatabase::open(temporary.path() / "rollback.db");

    expect(database.has_value(), "rollback database opens");

    if (!database) {
        return;
    }

    rlbs::JobRepository repository{*database};
    auto invalid = example_spec("duplicate environment");
    invalid.environment.push_back({.name = "FIRST", .value = "again"});

    const auto rejected = repository.submit(invalid);
    expect(!rejected, "duplicate environment submission is rejected");
    expect(!rejected && rejected.error().operation ==
                            rlbs::RepositoryOperation::insert_environment,
           "failed submission reports the environment insert");

    const auto pending_after_failure = repository.pending();
    expect(pending_after_failure && pending_after_failure->empty(),
           "failed submission leaves no partial job behind");

    const auto valid = repository.submit(example_spec("valid afterward"));
    expect(valid.has_value(), "repository still works after rollback");
    expect(valid && valid->queue_sequence == 1,
           "rolled back job does not consume a queue position");
}

void test_rejects_unknown_queue() {
    TemporaryDirectory temporary;
    auto database =
        rlbs::SqliteDatabase::open(temporary.path() / "unknown-queue.db");

    expect(database.has_value(), "unknown queue database opens");

    if (!database) {
        return;
    }

    rlbs::JobRepository repository{*database};
    auto spec = example_spec("wrong queue");
    spec.queue = "missing";
    const auto rejected = repository.submit(spec);

    expect(!rejected, "job cannot enter an unknown queue");
    expect(!rejected &&
               rejected.error().operation ==
                   rlbs::RepositoryOperation::validate_queue,
           "unknown queue failure identifies queue validation");
}

void test_transitions_store_results_and_events() {
    TemporaryDirectory temporary;
    auto database =
        rlbs::SqliteDatabase::open(temporary.path() / "transitions.db");

    expect(database.has_value(), "transition database opens");

    if (!database) {
        return;
    }

    rlbs::JobRepository repository{*database};
    const auto submitted = repository.submit(example_spec("transitioned"));

    expect(submitted.has_value(), "transition job submits");

    if (!submitted) {
        return;
    }

    const auto assigned = repository.transition(
        submitted->id, {
                           .state = rlbs::JobState::assigned,
                           .assigned_node = "local",
                           .result = std::nullopt,
                           .detail = "first-fit picked the local node",
                       });
    const auto starting = repository.transition(
        submitted->id, {
                           .state = rlbs::JobState::starting,
                           .assigned_node = std::nullopt,
                           .result = std::nullopt,
                           .detail = std::nullopt,
                       });
    const auto running = repository.transition(
        submitted->id, {
                           .state = rlbs::JobState::running,
                           .assigned_node = std::nullopt,
                           .result = std::nullopt,
                           .detail = std::nullopt,
                       });
    const auto completed = repository.transition(
        submitted->id, {
                           .state = rlbs::JobState::completed,
                           .assigned_node = std::nullopt,
                           .result =
                               rlbs::JobResult{
                                   .exit_code = 7,
                                   .terminating_signal = std::nullopt,
                                   .dumped_core = false,
                               },
                           .detail = "process exited",
                       });

    expect(assigned && assigned->assigned_node == "local",
           "assigned transition stores its node");
    expect(starting && starting->state == rlbs::JobState::starting,
           "starting transition succeeds");
    expect(running && running->state == rlbs::JobState::running,
           "running transition succeeds");
    expect(completed && completed->state == rlbs::JobState::completed,
           "completed transition succeeds");
    expect(completed && completed->result && completed->result->exit_code == 7,
           "completed transition returns its result");

    const auto loaded = repository.find(submitted->id);
    expect(loaded && *loaded, "transitioned job reloads");

    if (loaded && *loaded) {
        expect((*loaded)->state == rlbs::JobState::completed,
               "terminal state survives persistence");
        expect((*loaded)->assigned_node == "local",
               "assigned node survives later transitions");
        expect((*loaded)->result && (*loaded)->result->exit_code == 7,
               "process result survives persistence");
        expect((*loaded)->execution_time.has_value(),
               "execution time is available after the job ran");
    }

    const auto events = repository.events(submitted->id);
    expect(events && events->size() == 4,
           "every successful transition creates one event");

    if (events && events->size() == 4) {
        expect((*events)[0].state == rlbs::JobState::assigned,
               "event history starts with assignment");
        expect((*events)[1].state == rlbs::JobState::starting,
               "event history records startup");
        expect((*events)[2].state == rlbs::JobState::running,
               "event history records running");
        expect((*events)[3].state == rlbs::JobState::completed,
               "event history records completion");
        expect((*events)[0].detail == "first-fit picked the local node",
               "event detail survives persistence");
        expect(!(*events)[0].occurred_at.empty(),
               "event includes its database timestamp");
    }

    const auto pending = repository.pending();
    expect(pending && pending->empty(),
           "terminal job is no longer in the pending queue");
}

void test_invalid_transition_changes_nothing() {
    TemporaryDirectory temporary;
    auto database =
        rlbs::SqliteDatabase::open(temporary.path() / "invalid-state.db");

    expect(database.has_value(), "invalid transition database opens");

    if (!database) {
        return;
    }

    rlbs::JobRepository repository{*database};
    const auto submitted = repository.submit(example_spec("still pending"));

    if (!submitted) {
        expect(false, "invalid transition fixture submits");
        return;
    }

    const auto rejected = repository.transition(
        submitted->id, {
                           .state = rlbs::JobState::running,
                           .assigned_node = std::nullopt,
                           .result = std::nullopt,
                           .detail = std::nullopt,
                       });
    expect(!rejected, "pending job cannot jump straight to running");
    expect(!rejected && rejected.error().operation ==
                            rlbs::RepositoryOperation::validate_transition,
           "invalid state change reports transition validation");

    const auto loaded = repository.find(submitted->id);
    expect(loaded && *loaded && (*loaded)->state == rlbs::JobState::pending,
           "rejected transition leaves the job pending");

    const auto events = repository.events(submitted->id);
    expect(events && events->empty(),
           "rejected transition does not create an event");
}

void test_event_failure_rolls_back_state() {
    TemporaryDirectory temporary;
    const auto path = temporary.path() / "event-rollback.db";
    auto database = rlbs::SqliteDatabase::open(path);

    expect(database.has_value(), "event rollback database opens");

    if (!database) {
        return;
    }

    rlbs::JobRepository repository{*database};
    const auto submitted = repository.submit(example_spec("atomic transition"));

    if (!submitted) {
        expect(false, "event rollback fixture submits");
        return;
    }

    sqlite3* setup = nullptr;
    const int opened = sqlite3_open(path.c_str(), &setup);
    expect(opened == SQLITE_OK, "event failure setup opens");

    if (opened != SQLITE_OK) {
        if (setup != nullptr) {
            static_cast<void>(sqlite3_close(setup));
        }
        return;
    }

    constexpr const char* trigger = R"sql(
CREATE TRIGGER reject_job_event
BEFORE INSERT ON job_events
BEGIN
    SELECT RAISE(ABORT, 'event blocked for rollback test');
END;
)sql";
    expect(sqlite3_exec(setup, trigger, nullptr, nullptr, nullptr) == SQLITE_OK,
           "event failure trigger installs");
    static_cast<void>(sqlite3_close(setup));

    const auto rejected = repository.transition(
        submitted->id, {
                           .state = rlbs::JobState::assigned,
                           .assigned_node = "local",
                           .result = std::nullopt,
                           .detail = std::nullopt,
                       });
    expect(!rejected, "event insert failure rejects the transition");
    expect(!rejected && rejected.error().operation ==
                            rlbs::RepositoryOperation::insert_event,
           "event insert failure reports the event operation");

    const auto loaded = repository.find(submitted->id);
    expect(loaded && *loaded && (*loaded)->state == rlbs::JobState::pending,
           "failed event insert rolls the state update back");
    expect(loaded && *loaded && !(*loaded)->assigned_node,
           "failed event insert rolls the node assignment back");
}

} // namespace

int main() {
    test_submit_find_and_reopen();
    test_pending_jobs_keep_fifo_order();
    test_all_jobs_include_finished_jobs();
    test_failed_submission_rolls_back();
    test_rejects_unknown_queue();
    test_transitions_store_results_and_events();
    test_invalid_transition_changes_nothing();
    test_event_failure_rolls_back_state();

    if (failures == 0) {
        std::cout << "all job repository tests passed\n";
    }

    return failures == 0 ? 0 : 1;
}
