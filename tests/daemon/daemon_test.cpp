#include <rlbs/control/unix_socket.hpp>
#include <rlbs/daemon/config.hpp>
#include <rlbs/persistence/database.hpp>
#include <rlbs/persistence/job_repository.hpp>

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <sys/wait.h>
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
            (std::filesystem::temp_directory_path() / "rlbs-daemon-XXXXXX")
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

class DaemonProcess {
  public:
    explicit DaemonProcess(pid_t pid) : pid_{pid} {}

    DaemonProcess(const DaemonProcess&) = delete;
    DaemonProcess& operator=(const DaemonProcess&) = delete;

    ~DaemonProcess() {
        if (pid_ > 0) {
            static_cast<void>(::kill(pid_, SIGKILL));
            int status = 0;
            static_cast<void>(::waitpid(pid_, &status, 0));
        }
    }

    [[nodiscard]] bool stop() {
        if (pid_ <= 0 || ::kill(pid_, SIGTERM) < 0) {
            return false;
        }

        constexpr int attempts = 400;

        for (int attempt = 0; attempt < attempts; ++attempt) {
            int status = 0;
            const auto result = ::waitpid(pid_, &status, WNOHANG);

            if (result == pid_) {
                pid_ = -1;
                return WIFEXITED(status) && WEXITSTATUS(status) == 0;
            }

            if (result < 0 && errno != EINTR) {
                return false;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }

        // leave pid_ owned here so the destructor can force cleanup if graceful
        // shutdown somehow got stuck instead of leaking the test daemon
        return false;
    }

  private:
    pid_t pid_{-1};
};

struct CommandResult {
    int exit_code{127};
    std::string output;
};

[[nodiscard]] CommandResult
run_submit_command(const std::filesystem::path& executable,
                   const std::filesystem::path& socket_path,
                   const std::filesystem::path& working_directory) {
    int output_pipe[2]{};

    if (::pipe(output_pipe) < 0) {
        return {};
    }

    const pid_t child = ::fork();

    if (child == 0) {
        static_cast<void>(::close(output_pipe[0]));

        if (::dup2(output_pipe[1], STDOUT_FILENO) < 0) {
            ::_exit(126);
        }

        static_cast<void>(::close(output_pipe[1]));
        ::execl(executable.c_str(), executable.c_str(), "submit", "--socket",
                socket_path.c_str(), "--name", "cli-job", "--cpus", "2",
                "--memory-mb", "1024", "--cwd", working_directory.c_str(),
                "--env", "CLI_MESSAGE=from cli", "--", "/bin/sh", "-c",
                "printf \"$CLI_MESSAGE\\n\"", static_cast<char*>(nullptr));
        ::_exit(127);
    }

    static_cast<void>(::close(output_pipe[1]));

    if (child < 0) {
        static_cast<void>(::close(output_pipe[0]));
        return {};
    }

    std::string output;
    std::array<char, 256> buffer{};

    for (;;) {
        const auto received =
            ::read(output_pipe[0], buffer.data(), buffer.size());

        if (received > 0) {
            output.append(buffer.data(), static_cast<std::size_t>(received));
            continue;
        }
        if (received < 0 && errno == EINTR) {
            continue;
        }

        break;
    }

    static_cast<void>(::close(output_pipe[0]));
    int status = 0;

    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }

    return {
        .exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128,
        .output = std::move(output),
    };
}

[[nodiscard]] std::string read_file(const std::filesystem::path& path) {
    std::ifstream input{path};
    return {
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{},
    };
}

void test_config_parser() {
    const std::vector<std::string_view> arguments{
        "--database",
        "/tmp/custom.db",
        "--node-id",
        "head",
        "--socket",
        "/tmp/custom.sock",
        "--cpus",
        "12",
        "--memory-mb",
        "64000",
        "--gpus",
        "2",
        "--reserve-cpus",
        "2",
        "--reserve-memory-mb",
        "4000",
        "--reserve-gpus",
        "1",
        "--tick-ms",
        "25",
    };
    const auto parsed = rlbs::parse_daemon_config(arguments);

    expect(parsed.has_value(), "complete daemon config parses");

    if (parsed) {
        expect(parsed->database_path == "/tmp/custom.db",
               "database path parses");
        expect(parsed->node_id == "head", "node id parses");
        expect(parsed->socket_path == "/tmp/custom.sock", "socket path parses");
        expect(parsed->capacity.cpus == 12, "cpu capacity parses");
        expect(parsed->capacity.memory_mb == 64000, "memory capacity parses");
        expect(parsed->capacity.gpus == 2, "gpu capacity parses");
        expect(parsed->reserved.cpus == 2, "reserved cpus parse");
        expect(parsed->reserved.memory_mb == 4000, "reserved memory parses");
        expect(parsed->reserved.gpus == 1, "reserved gpus parse");
        expect(parsed->tick_interval == std::chrono::milliseconds{25},
               "tick interval parses");
    }

    const std::array<std::string_view, 4> over_reserved{"--cpus", "2",
                                                        "--reserve-cpus", "3"};
    expect(!rlbs::parse_daemon_config(over_reserved),
           "reservation above capacity is rejected");

    const std::array<std::string_view, 1> missing_value{"--database"};
    expect(!rlbs::parse_daemon_config(missing_value),
           "option without a value is rejected");

    const std::array<std::string_view, 2> unknown{"--mystery", "1"};
    expect(!rlbs::parse_daemon_config(unknown),
           "unknown daemon option is rejected");
}

void test_real_cli_submits_to_daemon(
    const std::filesystem::path& daemon_executable,
    const std::filesystem::path& cli_executable) {
    TemporaryDirectory temporary;
    const auto database_path = temporary.path() / "rlbs.db";
    const auto socket_path = temporary.path() / "rlbs.sock";
    auto database = rlbs::SqliteDatabase::open(database_path);

    expect(database.has_value(), "daemon integration database opens");

    if (!database) {
        return;
    }

    rlbs::JobRepository repository{*database};
    const pid_t child = ::fork();

    if (child == 0) {
        ::execl(daemon_executable.c_str(), daemon_executable.c_str(),
                "--database", database_path.c_str(), "--socket",
                socket_path.c_str(), "--cpus", "2", "--memory-mb", "4096",
                "--tick-ms", "5", static_cast<char*>(nullptr));
        ::_exit(127);
    }

    expect(child > 0, "daemon process starts");

    if (child <= 0) {
        return;
    }

    DaemonProcess daemon{child};
    bool socket_ready = false;

    for (int attempt = 0; attempt < 400; ++attempt) {
        if (std::filesystem::exists(socket_path)) {
            socket_ready = true;
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }

    expect(socket_ready, "real rlbsd creates its control socket");

    if (!socket_ready) {
        return;
    }

    const auto submitted =
        run_submit_command(cli_executable, socket_path, temporary.path());
    expect(submitted.exit_code == 0, "real rlbs submit exits successfully");
    expect(submitted.output == "submitted job 1\n",
           "real rlbs submit prints the assigned job id");

    bool completed = false;

    for (int attempt = 0; attempt < 400; ++attempt) {
        const auto loaded = repository.find(1);

        if (loaded && *loaded &&
            (*loaded)->state == rlbs::JobState::completed) {
            completed = true;
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }

    expect(completed, "real rlbsd completes the cli-submitted job");
    expect(read_file(temporary.path() / "cli-job.o1") == "from cli\n",
           "cli-submitted job writes its default stdout file");
    expect(std::filesystem::exists(temporary.path() / "cli-job.e1"),
           "cli-submitted job creates its default stderr file");
    expect(daemon.stop(), "rlbsd exits cleanly on sigterm");
}

} // namespace

int main(int argc, char* argv[]) {
    test_config_parser();

    if (argc == 3) {
        test_real_cli_submits_to_daemon(argv[1], argv[2]);
    } else {
        expect(false, "daemon test receives daemon and cli executable paths");
    }

    if (failures == 0) {
        std::cout << "all daemon tests passed\n";
    }

    return failures == 0 ? 0 : 1;
}
