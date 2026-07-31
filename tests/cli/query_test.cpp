#include <rlbs/cli/query.hpp>

#include <array>
#include <iostream>
#include <string_view>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "failed: " << message << '\n';
        ++failures;
    }
}

void test_queue_parser() {
    const std::array<std::string_view, 2> arguments{"--socket",
                                                    "/tmp/custom.sock"};
    const auto parsed = rlbs::parse_queue_command(arguments);

    expect(parsed && parsed->socket_path == "/tmp/custom.sock",
           "queue parses its socket");

    const std::array<std::string_view, 1> unknown{"surprise"};
    expect(!rlbs::parse_queue_command(unknown),
           "queue rejects unexpected arguments");
}

void test_status_parser() {
    const std::array<std::string_view, 3> arguments{
        "--socket",
        "/tmp/custom.sock",
        "42",
    };
    const auto parsed = rlbs::parse_status_command(arguments);

    expect(parsed && parsed->job_id == 42, "status parses the job id");
    expect(parsed && parsed->socket_path == "/tmp/custom.sock",
           "status parses its socket");

    const std::array<std::string_view, 1> zero{"0"};
    const std::array<std::string_view, 2> extra{"1", "2"};
    expect(!rlbs::parse_status_command(zero), "status rejects job id zero");
    expect(!rlbs::parse_status_command(extra),
           "status rejects a second job id");
    expect(!rlbs::parse_status_command({}), "status requires a job id");
}

void test_cancel_parser() {
    const std::array<std::string_view, 3> arguments{
        "42",
        "--socket",
        "/tmp/custom.sock",
    };
    const auto parsed = rlbs::parse_cancel_command(arguments);

    expect(parsed && parsed->job_id == 42, "cancel parses the job id");
    expect(parsed && parsed->socket_path == "/tmp/custom.sock",
           "cancel parses its socket");

    const std::array<std::string_view, 1> invalid{"not-an-id"};
    expect(!rlbs::parse_cancel_command(invalid),
           "cancel rejects a nonnumeric job id");
    expect(!rlbs::parse_cancel_command({}), "cancel requires a job id");
}

void test_nodes_parser() {
    const std::array<std::string_view, 2> arguments{"--socket",
                                                    "/tmp/custom.sock"};
    const auto parsed = rlbs::parse_nodes_command(arguments);

    expect(parsed && parsed->socket_path == "/tmp/custom.sock",
           "nodes parses its socket");

    const std::array<std::string_view, 1> unknown{"surprise"};
    expect(!rlbs::parse_nodes_command(unknown),
           "nodes rejects unexpected arguments");
}

void test_queue_format() {
    const std::vector<rlbs::JobSummary> jobs{
        {
            .id = 7,
            .name = "waiting job",
            .queue = "short",
            .state = rlbs::JobState::pending,
            .resources = {.cpus = 2, .memory_mb = 4096, .gpus = 0},
            .assigned_node = std::nullopt,
            .walltime = std::chrono::seconds{300},
            .execution_time = std::nullopt,
        },
        {
            .id = 8,
            .name = "running job",
            .queue = "long",
            .state = rlbs::JobState::running,
            .resources = {.cpus = 4, .memory_mb = 8192, .gpus = 1},
            .assigned_node = "head",
            .walltime = std::chrono::seconds{600},
            .execution_time = std::chrono::seconds{17},
        },
    };
    const auto output = rlbs::format_queue(jobs);

    expect(output.contains("job id"), "queue prints a header");
    expect(output.contains("7       pending"), "queue prints pending jobs");
    expect(output.contains("8       running"), "queue prints running jobs");
    expect(output.contains("head"), "queue prints assigned nodes");
    expect(output.contains("short"), "queue prints queue names");
    expect(output.contains("running job"), "queue prints job names");
    expect(output.contains("00:00:17"), "queue prints execution time");
    expect(output.contains("00:10:00"), "queue prints walltime");
}

void test_status_format() {
    const rlbs::Job job{
        .id = 9,
        .queue_sequence = 4,
        .spec =
            {
                .name = "detailed job",
                .resources = {.cpus = 3, .memory_mb = 1024, .gpus = 0},
                .argv = {"/bin/sh", "-c", "printf hello world"},
                .working_directory = "/tmp/work",
                .environment = {{.name = "HELLO", .value = "world"}},
                .inherit_environment = false,
                .stdout_path = "job.out",
                .stderr_path = std::nullopt,
                .append_output = true,
                .walltime = std::chrono::seconds{90},
                .queue = "short",
            },
        .state = rlbs::JobState::completed,
        .assigned_node = "head",
        .result =
            rlbs::JobResult{
                .exit_code = 7,
                .terminating_signal = std::nullopt,
                .dumped_core = false,
            },
        .execution_time = std::chrono::seconds{17},
    };
    const auto output = rlbs::format_status(job);

    expect(output.contains("job id: 9"), "status prints the job id");
    expect(output.contains("state: completed"), "status prints the state");
    expect(output.contains("queue: short"), "status prints the queue");
    expect(output.contains("node: head"), "status prints the node");
    expect(output.contains("command: \"/bin/sh\" \"-c\""),
           "status keeps command arguments separate");
    expect(output.contains("environment override: \"HELLO=world\""),
           "status prints stored environment overrides");
    expect(output.contains("exit code: 7"), "status prints the exit code");
    expect(output.contains("walltime: 00:01:30"),
           "status prints requested walltime");
    expect(output.contains("execution time: 00:00:17"),
           "status prints execution time");
    expect(output.contains("stderr: (daemon default)"),
           "status explains default output paths");
}

void test_nodes_format() {
    const std::vector<rlbs::NodeSummary> nodes{
        {
            .id = "head",
            .state = rlbs::NodeState::online,
            .total = {.cpus = 8, .memory_mb = 32768, .gpus = 2},
            .reserved = {.cpus = 2, .memory_mb = 4096, .gpus = 1},
            .allocated = {.cpus = 4, .memory_mb = 8192, .gpus = 0},
            .available = {.cpus = 2, .memory_mb = 20480, .gpus = 1},
        },
    };
    const auto output = rlbs::format_nodes(nodes);

    expect(output.contains("cpus t/r/u/a"),
           "nodes explains the resource column order");
    expect(output.contains("head"), "nodes prints the node id");
    expect(output.contains("online"), "nodes prints the node state");
    expect(output.contains("8/2/4/2"), "nodes prints cpu accounting");
    expect(output.contains("32768/4096/8192/20480"),
           "nodes prints memory accounting");
    expect(output.contains("2/1/0/1"), "nodes prints gpu accounting");
}

} // namespace

int main() {
    test_queue_parser();
    test_status_parser();
    test_cancel_parser();
    test_nodes_parser();
    test_queue_format();
    test_status_format();
    test_nodes_format();

    if (failures == 0) {
        std::cout << "all query cli tests passed\n";
    }

    return failures == 0 ? 0 : 1;
}
