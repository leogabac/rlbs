#include <rlbs/persistence/database.hpp>
#include <rlbs/persistence/job_repository.hpp>

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
}

void test_submit_find_and_reopen() {
    TemporaryDirectory temporary;
    const auto path = temporary.path() / "rlbs.db";
    const auto spec = example_spec("round trip");
    rlbs::JobId submitted_id = 0;

    {
        auto database = rlbs::SqliteDatabase::open(path);
        expect(database.has_value(), "repository database opens");

        if (!database) {
            return;
        }

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

} // namespace

int main() {
    test_submit_find_and_reopen();
    test_pending_jobs_keep_fifo_order();
    test_failed_submission_rolls_back();

    if (failures == 0) {
        std::cout << "all job repository tests passed\n";
    }

    return failures == 0 ? 0 : 1;
}
