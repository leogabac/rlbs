#include <rlbs/cli/pbs_script.hpp>

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
            (std::filesystem::temp_directory_path() / "rlbs-pbs-XXXXXX")
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

void test_complete_script() {
    TemporaryDirectory temporary;
    const auto script = temporary.path() / "python-job.pbs";
    write_file(script, R"pbs(#!/usr/bin/env python3
# this ordinary comment does not close the directive section
#PBS -N "python runtime"
#PBS -q short
#PBS -l select=1:ncpus=4:mem=2gb:ngpus=1
#PBS -l walltime=01:02:03
#PBS -d /tmp
#PBS -V
#PBS -v EXPLICIT=hello,FROM_SUBMIT
#PBS -o python.out
#PBS -e python.err
print("hello")
)pbs");
    const std::vector<rlbs::EnvironmentVariable> environment{
        {.name = "BASE", .value = "kept"},
        {.name = "FROM_SUBMIT", .value = "copied"},
        {.name = "EXPLICIT", .value = "old"},
    };
    const auto parsed =
        rlbs::parse_pbs_script(script, temporary.path(), environment);

    expect(parsed.has_value(), "complete pbs script parses");

    if (!parsed) {
        return;
    }

    expect(parsed->name == "python runtime", "pbs -N sets the job name");
    expect(parsed->queue == "short", "pbs -q sets the queue");
    expect(parsed->resources.cpus == 4, "pbs select sets ncpus");
    expect(parsed->resources.memory_mb == 2048, "pbs select converts memory");
    expect(parsed->resources.gpus == 1, "pbs select sets ngpus");
    expect(parsed->walltime == std::chrono::seconds{3723},
           "pbs walltime parses");
    expect(parsed->working_directory == "/tmp", "pbs -d sets the directory");
    expect(parsed->stdout_path == "python.out", "pbs -o sets stdout");
    expect(parsed->stderr_path == "python.err", "pbs -e sets stderr");
    expect(!parsed->inherit_environment,
           "pbs -V sends the submit environment explicitly");
    expect(parsed->argv.size() == 3 && parsed->argv[0] == "/usr/bin/env" &&
               parsed->argv[1] == "python3" &&
               parsed->argv[2] == std::filesystem::absolute(script).string(),
           "script shebang selects its interpreter");

    const auto* base = find_environment(*parsed, "BASE");
    const auto* copied = find_environment(*parsed, "FROM_SUBMIT");
    const auto* explicit_value = find_environment(*parsed, "EXPLICIT");
    expect(base && base->value == "kept", "pbs -V keeps client variables");
    expect(copied && copied->value == "copied",
           "pbs -v can copy a client variable by name");
    expect(explicit_value && explicit_value->value == "hello",
           "pbs -v explicit value overrides the client environment");
}

void test_direct_resources_and_defaults() {
    TemporaryDirectory temporary;
    const auto script = temporary.path() / "plain.pbs";
    write_file(script, R"pbs(#PBS -l ncpus=2,mem=512mb,ngpus=0
echo hello
)pbs");
    const auto parsed = rlbs::parse_pbs_script(
        script, temporary.path(), std::span<const rlbs::EnvironmentVariable>{});

    expect(parsed.has_value(), "direct pbs resources parse");

    if (parsed) {
        expect(parsed->name == "plain.pbs",
               "script filename is the default job name");
        expect(parsed->queue == "default",
               "pbs script defaults to the default queue");
        expect(parsed->resources.cpus == 2, "direct ncpus parses");
        expect(parsed->resources.memory_mb == 512, "direct memory parses");
        expect(parsed->argv.size() == 2 && parsed->argv[0] == "/bin/sh",
               "script without a shebang uses bin sh");
        expect(parsed->working_directory == temporary.path(),
               "script defaults to the submission directory");
    }
}

void test_unsupported_requests_are_rejected() {
    TemporaryDirectory temporary;
    const auto multi_node = temporary.path() / "multi.pbs";
    const auto joined = temporary.path() / "joined.pbs";
    const auto unknown = temporary.path() / "unknown.pbs";
    const auto misplaced = temporary.path() / "misplaced.pbs";

    write_file(multi_node, "#PBS -l select=2:ncpus=1\n/bin/true\n");
    write_file(joined, "#PBS -j oe\n/bin/true\n");
    write_file(unknown, "#PBS -l mystery=1\n/bin/true\n");
    write_file(misplaced, "echo started\n#PBS -N too-late\n");

    const std::span<const rlbs::EnvironmentVariable> environment;
    expect(!rlbs::parse_pbs_script(multi_node, temporary.path(), environment),
           "multi-node select is rejected");
    expect(!rlbs::parse_pbs_script(joined, temporary.path(), environment),
           "unsupported output joining is rejected");
    expect(!rlbs::parse_pbs_script(unknown, temporary.path(), environment),
           "unknown resources are rejected");
    expect(!rlbs::parse_pbs_script(misplaced, temporary.path(), environment),
           "directives after executable code are rejected");
}

} // namespace

int main() {
    test_complete_script();
    test_direct_resources_and_defaults();
    test_unsupported_requests_are_rejected();

    if (failures == 0) {
        std::cout << "all pbs script tests passed\n";
    }

    return failures == 0 ? 0 : 1;
}
