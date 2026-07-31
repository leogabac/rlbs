#include <rlbs/local/output_spool.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
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
            (std::filesystem::temp_directory_path() / "rlbs-spool-XXXXXX")
                .string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');

        if (const auto* created = ::mkdtemp(writable.data())) {
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
    std::ifstream input{path, std::ios::binary};
    return {
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{},
    };
}

void write_file(const std::filesystem::path& path, std::string_view contents,
                bool append = false) {
    std::ofstream output{
        path,
        std::ios::binary | (append ? std::ios::app : std::ios::trunc),
    };
    output << contents;
}

[[nodiscard]] rlbs::Job spool_job(const std::filesystem::path& directory) {
    return {
        .id = 17,
        .queue_sequence = 3,
        .spec =
            {
                .name = "spool test",
                .resources = {.cpus = 1, .memory_mb = 0, .gpus = 0},
                .argv = {"/bin/true"},
                .working_directory = directory,
                .environment = {},
                .inherit_environment = true,
                .stdout_path = "final.out",
                .stderr_path = "final.err",
                .append_output = false,
                .walltime = std::nullopt,
                .queue = "default",
            },
        .state = rlbs::JobState::running,
        .assigned_node = "local",
        .result = std::nullopt,
        .execution_time = std::nullopt,
        .owner =
            rlbs::JobOwner{
                .user_id = 1000,
                .group_id = 100,
            },
    };
}

void test_stages_private_output_at_the_end() {
    TemporaryDirectory temporary;
    const auto spool_root = temporary.path() / "spool";
    const auto job = spool_job(temporary.path());
    auto spool = rlbs::PreparedOutputSpool::create(spool_root, job);

    expect(spool.has_value(), "output spool prepares");

    if (!spool) {
        return;
    }

    write_file(spool->stdout_path(), "stdout bytes\n");
    write_file(spool->stderr_path(), "stderr bytes\n");

    expect(!std::filesystem::exists(temporary.path() / "final.out"),
           "final stdout is hidden while the job owns the spool");
    expect(spool->stage().has_value(), "finished output stages");
    expect(read_file(temporary.path() / "final.out") == "stdout bytes\n",
           "stdout reaches its requested destination");
    expect(read_file(temporary.path() / "final.err") == "stderr bytes\n",
           "stderr reaches its requested destination");
    expect(!std::filesystem::exists(spool->directory()),
           "successful staging removes the private spool");
}

void test_append_is_published_atomically() {
    TemporaryDirectory temporary;
    auto job = spool_job(temporary.path());
    job.spec.append_output = true;
    write_file(temporary.path() / "final.out", "older\n");
    auto spool =
        rlbs::PreparedOutputSpool::create(temporary.path() / "spool", job);

    expect(spool.has_value(), "append spool prepares");

    if (!spool) {
        return;
    }

    write_file(spool->stdout_path(), "newer\n");
    write_file(spool->stderr_path(), "");
    expect(spool->stage().has_value(), "append spool stages");
    expect(read_file(temporary.path() / "final.out") == "older\nnewer\n",
           "append keeps old output before the new spool");
}

void test_failed_stage_keeps_the_spool() {
    TemporaryDirectory temporary;
    auto job = spool_job(temporary.path());
    job.spec.stdout_path = "missing/final.out";
    auto spool =
        rlbs::PreparedOutputSpool::create(temporary.path() / "spool", job);

    expect(spool.has_value(), "failed-stage spool prepares");

    if (!spool) {
        return;
    }

    write_file(spool->stdout_path(), "do not lose this\n");
    const auto staged = spool->stage();

    expect(!staged, "missing destination directory rejects staging");
    expect(read_file(spool->stdout_path()) == "do not lose this\n",
           "failed staging leaves stdout recoverable");
    expect(std::filesystem::exists(spool->directory()),
           "failed staging leaves the private directory intact");

    std::filesystem::create_directory(temporary.path() / "missing");
    expect(spool->stage().has_value(),
           "staging retries once the destination becomes available");
    expect(read_file(temporary.path() / "missing/final.out") ==
               "do not lose this\n",
           "retried staging publishes the preserved bytes");
}

void test_joined_streams_share_one_spool() {
    TemporaryDirectory temporary;
    auto job = spool_job(temporary.path());
    job.spec.stderr_path = job.spec.stdout_path;
    auto spool =
        rlbs::PreparedOutputSpool::create(temporary.path() / "spool", job);

    expect(spool.has_value(), "joined-output spool prepares");

    if (!spool) {
        return;
    }

    expect(spool->stdout_path() == spool->stderr_path(),
           "joined output gives the runner one shared file");
    write_file(spool->stdout_path(), "stdout then stderr\n");
    expect(spool->stage().has_value(), "joined output stages once");
    expect(read_file(temporary.path() / "final.out") ==
               "stdout then stderr\n",
           "joined output reaches the shared destination");
}

} // namespace

int main() {
    test_stages_private_output_at_the_end();
    test_append_is_published_atomically();
    test_failed_stage_keeps_the_spool();
    test_joined_streams_share_one_spool();

    if (failures == 0) {
        std::cout << "all output spool tests passed\n";
    }

    return failures == 0 ? 0 : 1;
}
