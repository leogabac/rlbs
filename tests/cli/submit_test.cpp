#include <rlbs/cli/submit.hpp>

#include <array>
#include <filesystem>
#include <iostream>
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

void test_minimal_submit() {
    const std::array<std::string_view, 3> arguments{"--", "/bin/echo", "hello"};
    const auto command = rlbs::parse_submit_command(arguments);

    expect(command.has_value(), "minimal submit parses");

    if (command) {
        expect(command->socket_path == "/tmp/rlbs.sock",
               "minimal submit uses the default socket");
        expect(command->spec.name == "echo",
               "minimal submit derives its job name");
        expect(command->spec.resources.cpus == 1,
               "minimal submit requests one cpu");
        expect(command->spec.argv ==
                   std::vector<std::string>{"/bin/echo", "hello"},
               "minimal submit keeps argv");
        expect(command->spec.working_directory ==
                   std::filesystem::current_path(),
               "minimal submit uses the current directory");
    }
}

void test_full_submit() {
    const std::array<std::string_view, 24> arguments{
        "--socket",
        "/tmp/custom.sock",
        "--name",
        "full",
        "--cpus",
        "4",
        "--memory-mb",
        "8192",
        "--gpus",
        "1",
        "--cwd",
        "/tmp",
        "--env",
        "FIRST=one",
        "--env",
        "EMPTY=",
        "--no-inherit-env",
        "--stdout",
        "full.out",
        "--stderr",
        "full.err",
        "--append",
        "--",
        "/bin/true",
    };
    const auto command = rlbs::parse_submit_command(arguments);

    expect(command.has_value(), "full submit parses");

    if (command) {
        expect(command->socket_path == "/tmp/custom.sock",
               "full submit keeps the socket");
        expect(command->spec.name == "full", "full submit keeps the name");
        expect(command->spec.resources.cpus == 4, "full submit keeps cpus");
        expect(command->spec.resources.memory_mb == 8192,
               "full submit keeps memory");
        expect(command->spec.resources.gpus == 1, "full submit keeps gpus");
        expect(command->spec.working_directory == "/tmp",
               "full submit resolves cwd");
        expect(command->spec.environment.size() == 2,
               "full submit keeps environment variables");
        expect(!command->spec.inherit_environment,
               "full submit disables environment inheritance");
        expect(command->spec.stdout_path == "full.out",
               "full submit keeps stdout");
        expect(command->spec.stderr_path == "full.err",
               "full submit keeps stderr");
        expect(command->spec.append_output, "full submit enables append");
    }
}

void test_bad_submissions() {
    const std::array<std::string_view, 1> no_command{"--"};
    expect(!rlbs::parse_submit_command(no_command),
           "empty command is rejected");

    const std::array<std::string_view, 4> zero_cpus{"--cpus", "0", "--",
                                                    "/bin/true"};
    expect(!rlbs::parse_submit_command(zero_cpus),
           "zero cpu request is rejected");

    const std::array<std::string_view, 6> duplicate_environment{
        "--env", "SAME=one", "--env", "SAME=two", "--", "/bin/true"};
    expect(!rlbs::parse_submit_command(duplicate_environment),
           "duplicate environment is rejected");

    const std::array<std::string_view, 2> missing_delimiter{"/bin/true",
                                                            "argument"};
    expect(!rlbs::parse_submit_command(missing_delimiter),
           "missing command delimiter is rejected");
}

} // namespace

int main() {
    test_minimal_submit();
    test_full_submit();
    test_bad_submissions();

    if (failures == 0) {
        std::cout << "all submit parser tests passed\n";
    }

    return failures == 0 ? 0 : 1;
}
