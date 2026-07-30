#include <rlbs/cli/native_script.hpp>

#include <filesystem>
#include <fstream>
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
            (std::filesystem::temp_directory_path() / "rlbs-native-XXXXXX")
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

void write_file(const std::filesystem::path& path, std::string_view contents) {
    std::ofstream output{path};
    output << contents;
}

[[nodiscard]] const rlbs::EnvironmentVariable*
find_environment(const rlbs::JobSpec& spec, std::string_view name) {
    for (const auto& variable : spec.environment) {
        if (variable.name == name) {
            return &variable;
        }
    }

    return nullptr;
}

void test_complete_native_script() {
    TemporaryDirectory temporary;
    const auto script = temporary.path() / "complete.rlbs";
    write_file(script, R"rlbs(#!/usr/bin/env bash
# an ordinary comment can live around the header
#RLBS version = 1
#RLBS name = "native runtime"
#RLBS walltime = "01:02:03"
#RLBS resources.cpus = 4
#RLBS resources.memory_mb = 2048
#RLBS resources.gpus = 1
#RLBS working_directory = "/tmp"
#RLBS inherit_environment = false
#RLBS environment.RUN_MODE = "test\nmode"
#RLBS output.stdout = "native.out"
#RLBS output.stderr = 'native.err'
#RLBS output.append = true

printf 'hello\n'
)rlbs");
    const auto parsed =
        rlbs::parse_native_script(script, temporary.path());

    expect(parsed.has_value(), "complete native script parses");

    if (!parsed) {
        return;
    }

    expect(parsed->name == "native runtime", "native name parses");
    expect(parsed->walltime == std::chrono::seconds{3723},
           "native walltime parses");
    expect(parsed->resources.cpus == 4, "native cpus parse");
    expect(parsed->resources.memory_mb == 2048, "native memory parses");
    expect(parsed->resources.gpus == 1, "native gpus parse");
    expect(parsed->working_directory == "/tmp",
           "native working directory parses");
    expect(!parsed->inherit_environment,
           "native environment inheritance parses");
    expect(parsed->stdout_path == "native.out", "native stdout parses");
    expect(parsed->stderr_path == "native.err", "native stderr parses");
    expect(parsed->append_output, "native append parses");
    expect(parsed->argv.size() == 3 && parsed->argv[0] == "/usr/bin/env" &&
               parsed->argv[1] == "bash" &&
               parsed->argv[2] == std::filesystem::absolute(script).string(),
           "native shebang launches the original script");

    const auto* run_mode = find_environment(*parsed, "RUN_MODE");
    expect(run_mode && run_mode->value == "test\nmode",
           "native environment string escapes parse");
}

void test_native_defaults() {
    TemporaryDirectory temporary;
    const auto script = temporary.path() / "defaults.rlbs";
    write_file(script, "#RLBS version = 1\necho hello\n");
    const auto parsed =
        rlbs::parse_native_script(script, temporary.path());

    expect(parsed.has_value(), "minimal native script parses");

    if (parsed) {
        expect(parsed->name == "defaults.rlbs",
               "native script filename is the default name");
        expect(parsed->resources.cpus == 1,
               "native script defaults to one cpu");
        expect(parsed->working_directory == temporary.path(),
               "native script defaults to the submission directory");
        expect(parsed->argv.size() == 2 && parsed->argv[0] == "/bin/sh",
               "native script without shebang uses bin sh");
    }

    expect(rlbs::is_native_script_path("job.rlbs"),
           "rlbs extension selects the native parser");
    expect(rlbs::is_native_script_path("job.rlbs.sh"),
           "rlbs sh extension selects the native parser");
    expect(!rlbs::is_native_script_path("job.pbs"),
           "pbs extension stays on the pbs parser");
}

void test_bad_native_scripts_are_rejected() {
    TemporaryDirectory temporary;
    const auto missing_version = temporary.path() / "missing.rlbs";
    const auto duplicate = temporary.path() / "duplicate.rlbs";
    const auto unknown = temporary.path() / "unknown.rlbs";
    const auto late = temporary.path() / "late.rlbs";
    const auto bad_type = temporary.path() / "type.rlbs";
    const auto bad_environment = temporary.path() / "environment.rlbs";

    write_file(missing_version, "#RLBS name = \"no version\"\n/bin/true\n");
    write_file(duplicate,
               "#RLBS version = 1\n#RLBS resources.cpus = 1\n"
               "#RLBS resources.cpus = 2\n/bin/true\n");
    write_file(unknown,
               "#RLBS version = 1\n#RLBS magic = true\n/bin/true\n");
    write_file(late,
               "#RLBS version = 1\necho started\n#RLBS name = \"late\"\n");
    write_file(bad_type,
               "#RLBS version = 1\n#RLBS output.append = \"yes\"\n");
    write_file(bad_environment,
               "#RLBS version = 1\n#RLBS environment.BAD-NAME = \"x\"\n");

    expect(!rlbs::parse_native_script(missing_version, temporary.path()),
           "native script requires its format version");
    expect(!rlbs::parse_native_script(duplicate, temporary.path()),
           "native script rejects duplicate keys");
    expect(!rlbs::parse_native_script(unknown, temporary.path()),
           "native script rejects unknown keys");
    expect(!rlbs::parse_native_script(late, temporary.path()),
           "native script rejects directives after shell code");
    expect(!rlbs::parse_native_script(bad_type, temporary.path()),
           "native script rejects the wrong value type");
    expect(!rlbs::parse_native_script(bad_environment, temporary.path()),
           "native script rejects invalid environment names");
}

} // namespace

int main() {
    test_complete_native_script();
    test_native_defaults();
    test_bad_native_scripts_are_rejected();

    if (failures == 0) {
        std::cout << "all native script tests passed\n";
    }

    return failures == 0 ? 0 : 1;
}
