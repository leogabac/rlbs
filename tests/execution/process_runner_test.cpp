#include <rlbs/execution/process_runner.hpp>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <pwd.h>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

int failures = 0;
int child_marker_fd = -1;

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
            (std::filesystem::temp_directory_path() / "rlbs-runner-XXXXXX")
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

template <typename Predicate>
[[nodiscard]] bool wait_until(Predicate predicate) {
    constexpr auto timeout = std::chrono::seconds{2};
    constexpr auto interval = std::chrono::milliseconds{10};
    const auto deadline = std::chrono::steady_clock::now() + timeout;

    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }

        std::this_thread::sleep_for(interval);
    }

    return predicate();
}

void child_term_handler(int) {
    constexpr char marker = 'x';

    if (child_marker_fd >= 0) {
        static_cast<void>(::write(child_marker_fd, &marker, sizeof(marker)));
    }

    ::_exit(0);
}

int run_group_helper(const std::filesystem::path& ready_path,
                     const std::filesystem::path& marker_path) {
    child_marker_fd =
        ::open(marker_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);

    if (child_marker_fd < 0) {
        return 20;
    }

    int readiness_pipe[2]{};

    if (::pipe(readiness_pipe) < 0) {
        return 21;
    }

    const pid_t child = ::fork();

    if (child < 0) {
        return 22;
    }

    if (child == 0) {
        static_cast<void>(::close(readiness_pipe[0]));

        struct sigaction action{};
        action.sa_handler = child_term_handler;
        static_cast<void>(::sigemptyset(&action.sa_mask));
        action.sa_flags = 0;

        if (::sigaction(SIGTERM, &action, nullptr) < 0) {
            ::_exit(23);
        }

        constexpr char ready = 'r';
        static_cast<void>(::write(readiness_pipe[1], &ready, sizeof(ready)));
        static_cast<void>(::close(readiness_pipe[1]));

        for (;;) {
            ::pause();
        }
    }

    static_cast<void>(::close(readiness_pipe[1]));
    char ready = '\0';
    const auto readiness = ::read(readiness_pipe[0], &ready, sizeof(ready));
    static_cast<void>(::close(readiness_pipe[0]));

    if (readiness != sizeof(ready) || ready != 'r') {
        return 24;
    }

    std::ofstream ready_file{ready_path};
    ready_file << child << '\n';
    ready_file.close();

    for (;;) {
        ::pause();
    }
}

int run_helper(int argc, char* argv[]) {
    const std::string_view mode{argv[1]};

    if (mode == "--capture") {
        std::cout << std::filesystem::current_path().string() << '\n';

        for (int index = 2; index < argc; ++index) {
            std::cout << '[' << argv[index] << "]\n";
        }

        if (const char* value = std::getenv("RLBS_TEST_VALUE")) {
            std::cerr << value << '\n';
        }

        return 7;
    }

    if (mode == "--signal") {
        std::raise(SIGUSR1);
        return 25;
    }

    if (mode == "--identity") {
        std::cout << ::geteuid() << ' ' << ::getegid() << '\n';
        return 0;
    }

    if (mode == "--ignore-term" && argc == 3) {
        struct sigaction action{};
        action.sa_handler = SIG_IGN;
        static_cast<void>(::sigemptyset(&action.sa_mask));
        action.sa_flags = 0;

        if (::sigaction(SIGTERM, &action, nullptr) < 0) {
            return 27;
        }

        std::ofstream ready_file{argv[2]};
        ready_file << "ready\n";
        ready_file.close();

        for (;;) {
            ::pause();
        }
    }

    if (mode == "--group" && argc == 4) {
        return run_group_helper(argv[2], argv[3]);
    }

    return 26;
}

void test_validation() {
    const auto launched = rlbs::LocalProcessRunner{}.launch({});

    expect(!launched, "empty argv is rejected");
    expect(!launched &&
               launched.error().operation == rlbs::ProcessOperation::validate,
           "validation error identifies the failed operation");
}

void test_missing_executable() {
    rlbs::ProcessSpec spec{
        .argv = {"/definitely/not/an/rlbs/executable"},
        .working_directory = std::nullopt,
        .environment = {},
        .inherit_environment = true,
        .stdout_path = std::nullopt,
        .stderr_path = std::nullopt,
        .append_output = false,
        .run_as = std::nullopt,
    };
    const auto launched = rlbs::LocalProcessRunner{}.launch(spec);

    expect(!launched, "missing executable fails during launch");
    expect(!launched &&
               launched.error().operation == rlbs::ProcessOperation::execute,
           "exec failure is reported to the parent");
    expect(!launched && launched.error().system_error == ENOENT,
           "missing executable keeps its errno");
}

void test_execution_and_redirection(const std::filesystem::path& self) {
    TemporaryDirectory temporary;
    const auto stdout_path = temporary.path() / "stdout.txt";
    const auto stderr_path = temporary.path() / "stderr.txt";
    rlbs::ProcessSpec spec{
        .argv = {self.string(), "--capture", "hello world", ""},
        .working_directory = temporary.path(),
        .environment = {{"RLBS_TEST_VALUE", "environment works"}},
        .inherit_environment = true,
        .stdout_path = stdout_path.filename(),
        .stderr_path = stderr_path.filename(),
        .append_output = false,
        .run_as = std::nullopt,
    };
    const rlbs::LocalProcessRunner runner;
    auto launched = runner.launch(spec);

    expect(launched.has_value(), "valid process launches");

    if (!launched) {
        return;
    }

    auto handle = std::move(*launched);
    const auto moved_from = runner.poll(*launched);
    expect(!moved_from && moved_from.error().operation ==
                              rlbs::ProcessOperation::validate_handle,
           "moved-from handle cannot reap some unrelated child");

    const auto result = runner.wait(handle);

    expect(result.has_value(), "launched process can be waited");
    expect(result && result->exit_code == 7, "normal exit code is captured");
    expect(result && !result->terminating_signal,
           "normal exit does not invent a signal");
    expect(handle.finished(), "wait marks the handle as finished");

    const auto repeated = runner.wait(handle);
    expect(repeated && repeated->exit_code == 7,
           "repeated wait uses the cached result");

    const auto stdout_text = read_file(stdout_path);
    const auto stderr_text = read_file(stderr_path);
    expect(stdout_text.contains(temporary.path().string()),
           "child receives the requested working directory");
    expect(stdout_text.contains("[hello world]\n[]\n"),
           "argv survives spaces and empty arguments");
    expect(stderr_text == "environment works\n",
           "environment override reaches the child");
}

void test_signal_result(const std::filesystem::path& self) {
    const rlbs::LocalProcessRunner runner;
    auto launched = runner.launch({
        .argv = {self.string(), "--signal"},
        .working_directory = std::nullopt,
        .environment = {},
        .inherit_environment = true,
        .stdout_path = std::nullopt,
        .stderr_path = std::nullopt,
        .append_output = false,
        .run_as = std::nullopt,
    });

    expect(launched.has_value(), "signal helper launches");

    if (!launched) {
        return;
    }

    auto handle = std::move(*launched);
    const auto result = runner.wait(handle);
    expect(result && result->terminating_signal == SIGUSR1,
           "terminating signal is captured");
    expect(result && !result->exit_code,
           "signalled process does not invent an exit code");
}

void test_path_search() {
    const rlbs::LocalProcessRunner runner;
    auto launched = runner.launch({
        .argv = {"true"},
        .working_directory = std::nullopt,
        .environment = {{"PATH", "/bin:/usr/bin"}},
        .inherit_environment = false,
        .stdout_path = std::nullopt,
        .stderr_path = std::nullopt,
        .append_output = false,
        .run_as = std::nullopt,
    });

    expect(launched.has_value(),
           "executable is found through the supplied path");

    if (!launched) {
        return;
    }

    auto handle = std::move(*launched);
    const auto result = runner.wait(handle);
    expect(result && result->exit_code == 0,
           "path-resolved executable completes normally");
}

void test_process_group_cancellation(const std::filesystem::path& self) {
    TemporaryDirectory temporary;
    const auto ready_path = temporary.path() / "ready";
    const auto marker_path = temporary.path() / "child-terminated";
    const rlbs::LocalProcessRunner runner;
    auto launched = runner.launch({
        .argv =
            {
                self.string(),
                "--group",
                ready_path.string(),
                marker_path.string(),
            },
        .working_directory = std::nullopt,
        .environment = {},
        .inherit_environment = true,
        .stdout_path = std::nullopt,
        .stderr_path = std::nullopt,
        .append_output = false,
        .run_as = std::nullopt,
    });

    expect(launched.has_value(), "process-group helper launches");

    if (!launched) {
        return;
    }

    auto handle = std::move(*launched);
    const bool group_ready = wait_until(
        [&ready_path] { return std::filesystem::exists(ready_path); });
    expect(group_ready, "helper child joins the process group");

    const auto before_cancel = runner.poll(handle);
    expect(before_cancel && !*before_cancel,
           "poll reports a running process without blocking");

    const auto terminated = runner.terminate(handle);
    expect(terminated.has_value(), "terminate signals the process group");

    const auto result = runner.wait(handle);
    expect(result && result->terminating_signal == SIGTERM,
           "process-group leader records cancellation signal");

    const bool child_was_signalled = wait_until([&marker_path] {
        std::error_code ignored;
        const auto size = std::filesystem::file_size(marker_path, ignored);
        return !ignored && size > 0;
    });
    expect(child_was_signalled,
           "cancellation reaches child processes in the same group");
}

void test_force_kill(const std::filesystem::path& self) {
    TemporaryDirectory temporary;
    const auto ready_path = temporary.path() / "ignore-term-ready";
    const rlbs::LocalProcessRunner runner;
    auto launched = runner.launch({
        .argv = {self.string(), "--ignore-term", ready_path.string()},
        .working_directory = std::nullopt,
        .environment = {},
        .inherit_environment = true,
        .stdout_path = std::nullopt,
        .stderr_path = std::nullopt,
        .append_output = false,
        .run_as = std::nullopt,
    });

    expect(launched.has_value(), "force-kill helper launches");

    if (!launched) {
        return;
    }

    auto handle = std::move(*launched);
    const bool ready = wait_until(
        [&ready_path] { return std::filesystem::exists(ready_path); });
    expect(ready, "force-kill helper installs its signal handler");

    const auto terminated = runner.terminate(handle);
    expect(terminated.has_value(), "term can be sent before escalation");
    std::this_thread::sleep_for(std::chrono::milliseconds{20});

    const auto still_running = runner.poll(handle);
    expect(still_running && !*still_running,
           "ignored term leaves process running");

    const auto killed = runner.force_kill(handle);
    expect(killed.has_value(), "kill can be sent to the process group");

    const auto result = runner.wait(handle);
    expect(result && result->terminating_signal == SIGKILL,
           "forced cancellation records sigkill");
}

void test_execution_identity(const std::filesystem::path& self) {
    TemporaryDirectory temporary;
    const auto output_path = temporary.path() / "identity.txt";
    rlbs::JobOwner requested{
        .user_id = static_cast<std::uint32_t>(::geteuid()),
        .group_id = static_cast<std::uint32_t>(::getegid()),
    };

    // when the suite itself is root, use nobody for one real privilege drop.
    // normal developer runs still exercise the same-user no-op path without
    // needing sudo just to run the test suite.
    if (::geteuid() == 0) {
        if (const passwd* nobody = ::getpwnam("nobody");
            nobody != nullptr && nobody->pw_uid != 0) {
            requested.user_id = static_cast<std::uint32_t>(nobody->pw_uid);
            requested.group_id = static_cast<std::uint32_t>(nobody->pw_gid);
            std::filesystem::permissions(
                temporary.path(),
                std::filesystem::perms::owner_all |
                    std::filesystem::perms::group_read |
                    std::filesystem::perms::group_exec |
                    std::filesystem::perms::others_read |
                    std::filesystem::perms::others_exec);
        }
    }

    const rlbs::LocalProcessRunner runner;
    auto launched = runner.launch({
        .argv = {self.string(), "--identity"},
        .working_directory = temporary.path(),
        .environment = {},
        .inherit_environment = true,
        .stdout_path = output_path,
        .stderr_path = std::nullopt,
        .append_output = false,
        .run_as = requested,
    });

    expect(launched.has_value(), "process launches with a requested identity");

    if (!launched) {
        return;
    }

    auto handle = std::move(*launched);
    const auto result = runner.wait(handle);
    expect(result && result->exit_code == 0,
           "identity helper exits normally");
    expect(read_file(output_path) == std::to_string(requested.user_id) + " " +
                                         std::to_string(requested.group_id) +
                                         "\n",
           "child runs with the requested uid and gid");
}

void test_unprivileged_identity_switch_is_rejected() {
    if (::geteuid() == 0) {
        return;
    }

    const auto current_uid = static_cast<std::uint32_t>(::geteuid());
    const auto other_uid =
        current_uid == std::numeric_limits<std::uint32_t>::max()
            ? current_uid - 1
            : current_uid + 1;
    const auto launched = rlbs::LocalProcessRunner{}.launch({
        .argv = {"/bin/true"},
        .working_directory = std::nullopt,
        .environment = {},
        .inherit_environment = true,
        .stdout_path = std::nullopt,
        .stderr_path = std::nullopt,
        .append_output = false,
        .run_as =
            rlbs::JobOwner{
                .user_id = other_uid,
                .group_id = static_cast<std::uint32_t>(::getegid()),
            },
    });

    expect(!launched, "unprivileged runner rejects another uid");
    expect(!launched &&
               launched.error().operation ==
                   rlbs::ProcessOperation::resolve_identity,
           "identity rejection names the account-resolution step");
    expect(!launched && launched.error().system_error == EPERM,
           "identity rejection keeps the permission error");
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc > 1 && std::string_view{argv[1]}.starts_with("--")) {
        return run_helper(argc, argv);
    }

    const auto self = std::filesystem::absolute(argv[0]);
    test_validation();
    test_missing_executable();
    test_execution_and_redirection(self);
    test_signal_result(self);
    test_path_search();
    test_process_group_cancellation(self);
    test_force_kill(self);
    test_execution_identity(self);
    test_unprivileged_identity_switch_is_rejected();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }

    std::cout << "all execution tests passed\n";
    return 0;
}
