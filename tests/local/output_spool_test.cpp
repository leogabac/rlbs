#include <rlbs/local/output_spool.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <pwd.h>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>
#include <sys/stat.h>

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

[[nodiscard]] rlbs::JobOwner current_owner() {
    return {
        .user_id = static_cast<std::uint32_t>(::geteuid()),
        .group_id = static_cast<std::uint32_t>(::getegid()),
    };
}

[[nodiscard]] rlbs::Job
spool_job(const std::filesystem::path& directory,
          rlbs::JobOwner owner = current_owner()) {
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
        .owner = owner,
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

    struct stat published{};
    expect(::stat((temporary.path() / "final.out").c_str(), &published) == 0,
           "published stdout can be inspected");
    expect(static_cast<std::uint32_t>(published.st_uid) ==
                   job.owner->user_id &&
               static_cast<std::uint32_t>(published.st_gid) ==
                   job.owner->group_id,
           "published stdout belongs to the job owner");
    expect((published.st_mode & 0777) == 0600,
           "new output starts with conservative owner-only permissions");
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

void test_owner_permissions_reject_the_destination() {
    TemporaryDirectory temporary;
    const auto forbidden = temporary.path() / "read-only";
    std::filesystem::create_directory(forbidden);
    auto owner = current_owner();

    if (::geteuid() == 0) {
        const passwd* nobody = ::getpwnam("nobody");

        if (nobody == nullptr || nobody->pw_uid == 0) {
            return;
        }

        owner = {
            .user_id = static_cast<std::uint32_t>(nobody->pw_uid),
            .group_id = static_cast<std::uint32_t>(nobody->pw_gid),
        };
        std::filesystem::permissions(
            temporary.path(),
            std::filesystem::perms::owner_all |
                std::filesystem::perms::group_read |
                std::filesystem::perms::group_exec |
                std::filesystem::perms::others_read |
                std::filesystem::perms::others_exec);
        expect(::chown(forbidden.c_str(), nobody->pw_uid, nobody->pw_gid) == 0,
               "root permission test gives nobody the directory");
    }

    std::filesystem::permissions(
        forbidden, std::filesystem::perms::owner_read |
                       std::filesystem::perms::owner_exec);
    auto job = spool_job(forbidden, owner);
    auto spool =
        rlbs::PreparedOutputSpool::create(temporary.path() / "spool", job);

    expect(spool.has_value(), "permission-test spool prepares");

    if (!spool) {
        return;
    }

    write_file(spool->stdout_path(), "still recoverable\n");
    const auto staged = spool->stage();
    expect(!staged, "owner cannot publish into a read-only directory");
    expect(!staged &&
               staged.error().operation ==
                   rlbs::OutputSpoolOperation::create_stage_file,
           "permission failure identifies temporary-file creation");
    expect(read_file(spool->stdout_path()) == "still recoverable\n",
           "permission failure keeps the private output");
}

void test_root_publishes_as_an_unprivileged_owner() {
    if (::geteuid() != 0) {
        return;
    }

    const passwd* nobody = ::getpwnam("nobody");

    if (nobody == nullptr || nobody->pw_uid == 0) {
        return;
    }

    TemporaryDirectory temporary;
    const auto destination = temporary.path() / "owner-output";
    std::filesystem::create_directory(destination);

    // nobody needs traversal through the test root and ownership of the final
    // directory. the spool itself deliberately stays private to root.
    std::filesystem::permissions(
        temporary.path(),
        std::filesystem::perms::owner_all |
            std::filesystem::perms::group_read |
            std::filesystem::perms::group_exec |
            std::filesystem::perms::others_read |
            std::filesystem::perms::others_exec);
    expect(::chown(destination.c_str(), nobody->pw_uid, nobody->pw_gid) == 0,
           "root test gives nobody a destination directory");

    const rlbs::JobOwner owner{
        .user_id = static_cast<std::uint32_t>(nobody->pw_uid),
        .group_id = static_cast<std::uint32_t>(nobody->pw_gid),
    };
    auto job = spool_job(destination, owner);
    auto spool =
        rlbs::PreparedOutputSpool::create(temporary.path() / "spool", job);

    expect(spool.has_value(), "cross-user spool prepares");

    if (!spool) {
        return;
    }

    write_file(spool->stdout_path(), "owned by nobody\n");
    write_file(spool->stderr_path(), "");
    expect(spool->stage().has_value(),
           "root publisher stages through the owner identity");

    struct stat published{};
    expect(::stat((destination / "final.out").c_str(), &published) == 0,
           "cross-user output can be inspected");
    expect(published.st_uid == nobody->pw_uid &&
               published.st_gid == nobody->pw_gid,
           "cross-user output is really owned by the requested account");
}

} // namespace

int main() {
    test_stages_private_output_at_the_end();
    test_append_is_published_atomically();
    test_failed_stage_keeps_the_spool();
    test_joined_streams_share_one_spool();
    test_owner_permissions_reject_the_destination();
    test_root_publishes_as_an_unprivileged_owner();

    if (failures == 0) {
        std::cout << "all output spool tests passed\n";
    }

    return failures == 0 ? 0 : 1;
}
