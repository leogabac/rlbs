#include <rlbs/execution/process_runner.hpp>

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }

    std::cout << "all execution tests passed\n";
    return 0;
}
