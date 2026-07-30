#include <rlbs/cli/query.hpp>

#include <rlbs/cli/duration.hpp>

#include <charconv>
#include <iomanip>
#include <sstream>
#include <system_error>
#include <utility>

namespace rlbs {
namespace {

[[nodiscard]] std::string_view state_name(JobState state) {
    switch (state) {
    case JobState::pending:
        return "pending";
    case JobState::assigned:
        return "assigned";
    case JobState::starting:
        return "starting";
    case JobState::running:
        return "running";
    case JobState::completed:
        return "completed";
    case JobState::failed:
        return "failed";
    case JobState::cancelled:
        return "cancelled";
    }

    return "unknown";
}

[[nodiscard]] std::expected<std::string_view, std::string>
take_value(std::span<const std::string_view> arguments, std::size_t& index) {
    if (index + 1 >= arguments.size()) {
        return std::unexpected{std::string{arguments[index]} +
                               " needs a value"};
    }

    ++index;
    return arguments[index];
}

[[nodiscard]] std::expected<JobId, std::string>
parse_job_id(std::string_view value) {
    JobId job_id = 0;
    const auto parsed =
        std::from_chars(value.data(), value.data() + value.size(), job_id);

    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
        job_id == 0) {
        return std::unexpected{"job id needs to be a positive integer"};
    }

    return job_id;
}

void write_command(std::ostringstream& output,
                   const std::vector<std::string>& arguments) {
    // quote every argument so spaces and empty strings stay visible. this is
    // display text, not a promise that copying it back into a shell is magic
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        if (index != 0) {
            output << ' ';
        }

        output << std::quoted(arguments[index]);
    }
}

[[nodiscard]] std::string_view node_state_name(NodeState state) {
    switch (state) {
    case NodeState::online:
        return "online";
    case NodeState::draining:
        return "draining";
    case NodeState::offline:
        return "offline";
    }

    return "unknown";
}

template <typename Integer>
[[nodiscard]] std::string resource_cell(Integer total, Integer reserved,
                                        Integer allocated, Integer available) {
    return std::to_string(total) + '/' + std::to_string(reserved) + '/' +
           std::to_string(allocated) + '/' + std::to_string(available);
}

} // namespace

std::expected<QueueCommand, std::string>
parse_queue_command(std::span<const std::string_view> arguments) {
    QueueCommand command;

    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const auto option = arguments[index];

        if (option == "--help" || option == "-h") {
            command.show_help = true;
            continue;
        }
        if (option != "--socket") {
            return std::unexpected{"unknown queue option: " +
                                   std::string{option}};
        }

        auto value = take_value(arguments, index);

        if (!value) {
            return std::unexpected{std::move(value.error())};
        }

        command.socket_path = *value;
    }

    if (command.socket_path.empty()) {
        return std::unexpected{"--socket cannot be empty"};
    }

    return command;
}

std::expected<StatusCommand, std::string>
parse_status_command(std::span<const std::string_view> arguments) {
    StatusCommand command;
    bool found_job_id = false;

    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const auto argument = arguments[index];

        if (argument == "--help" || argument == "-h") {
            command.show_help = true;
            continue;
        }
        if (argument == "--socket") {
            auto value = take_value(arguments, index);

            if (!value) {
                return std::unexpected{std::move(value.error())};
            }

            command.socket_path = *value;
            continue;
        }
        if (argument.starts_with('-')) {
            return std::unexpected{"unknown status option: " +
                                   std::string{argument}};
        }
        if (found_job_id) {
            return std::unexpected{"status accepts exactly one job id"};
        }

        auto job_id = parse_job_id(argument);

        if (!job_id) {
            return std::unexpected{std::move(job_id.error())};
        }

        command.job_id = *job_id;
        found_job_id = true;
    }

    if (command.show_help) {
        return command;
    }
    if (!found_job_id) {
        return std::unexpected{"status needs a job id"};
    }
    if (command.socket_path.empty()) {
        return std::unexpected{"--socket cannot be empty"};
    }

    return command;
}

std::expected<CancelCommand, std::string>
parse_cancel_command(std::span<const std::string_view> arguments) {
    CancelCommand command;
    bool found_job_id = false;

    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const auto argument = arguments[index];

        if (argument == "--help" || argument == "-h") {
            command.show_help = true;
            continue;
        }
        if (argument == "--socket") {
            auto value = take_value(arguments, index);

            if (!value) {
                return std::unexpected{std::move(value.error())};
            }

            command.socket_path = *value;
            continue;
        }
        if (argument.starts_with('-')) {
            return std::unexpected{"unknown cancel option: " +
                                   std::string{argument}};
        }
        if (found_job_id) {
            return std::unexpected{"cancel accepts exactly one job id"};
        }

        auto job_id = parse_job_id(argument);

        if (!job_id) {
            return std::unexpected{std::move(job_id.error())};
        }

        command.job_id = *job_id;
        found_job_id = true;
    }

    if (command.show_help) {
        return command;
    }
    if (!found_job_id) {
        return std::unexpected{"cancel needs a job id"};
    }
    if (command.socket_path.empty()) {
        return std::unexpected{"--socket cannot be empty"};
    }

    return command;
}

std::expected<NodesCommand, std::string>
parse_nodes_command(std::span<const std::string_view> arguments) {
    NodesCommand command;

    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const auto option = arguments[index];

        if (option == "--help" || option == "-h") {
            command.show_help = true;
            continue;
        }
        if (option != "--socket") {
            return std::unexpected{"unknown nodes option: " +
                                   std::string{option}};
        }

        auto value = take_value(arguments, index);

        if (!value) {
            return std::unexpected{std::move(value.error())};
        }

        command.socket_path = *value;
    }

    if (command.socket_path.empty()) {
        return std::unexpected{"--socket cannot be empty"};
    }

    return command;
}

std::string format_queue(const std::vector<JobSummary>& jobs) {
    std::ostringstream output;
    output << std::left << std::setw(8) << "job id" << std::setw(12) << "state"
           << std::right << std::setw(6) << "cpus" << std::setw(12)
           << "memory mb" << std::setw(6) << "gpus" << "  " << std::left
           << std::setw(12) << "time" << std::setw(12) << "walltime"
           << std::setw(16) << "node" << "name\n";

    for (const auto& job : jobs) {
        output << std::left << std::setw(8) << job.id << std::setw(12)
               << state_name(job.state) << std::right << std::setw(6)
               << job.resources.cpus << std::setw(12) << job.resources.memory_mb
               << std::setw(6) << job.resources.gpus << "  " << std::left
               << std::setw(12)
               << (job.execution_time ? format_duration(*job.execution_time)
                                      : "-")
               << std::setw(12)
               << (job.walltime ? format_duration(*job.walltime) : "-")
               << std::setw(16) << job.assigned_node.value_or("-") << job.name
               << '\n';
    }

    return output.str();
}

std::string format_status(const Job& job) {
    std::ostringstream output;
    output << "job id: " << job.id << '\n'
           << "name: " << job.spec.name << '\n'
           << "state: " << state_name(job.state) << '\n'
           << "queue sequence: " << job.queue_sequence << '\n'
           << "node: " << job.assigned_node.value_or("-") << '\n'
           << "cpus: " << job.spec.resources.cpus << '\n'
           << "memory mb: " << job.spec.resources.memory_mb << '\n'
           << "gpus: " << job.spec.resources.gpus << '\n'
           << "walltime: "
           << (job.spec.walltime ? format_duration(*job.spec.walltime)
                                 : "unlimited")
           << '\n'
           << "execution time: "
           << (job.execution_time ? format_duration(*job.execution_time)
                                  : "not started")
           << '\n'
           << "working directory: " << job.spec.working_directory.string()
           << '\n'
           << "stdout: "
           << (job.spec.stdout_path ? job.spec.stdout_path->string()
                                    : "(daemon default)")
           << '\n'
           << "stderr: "
           << (job.spec.stderr_path ? job.spec.stderr_path->string()
                                    : "(daemon default)")
           << '\n'
           << "output mode: "
           << (job.spec.append_output ? "append" : "truncate") << '\n'
           << "environment base: "
           << (job.spec.inherit_environment ? "inherit" : "clean") << '\n';

    for (const auto& variable : job.spec.environment) {
        // these are only the explicitly stored overrides. inherited daemon
        // variables are intentionally not copied into sqlite by the thousand
        output << "environment override: "
               << std::quoted(variable.name + '=' + variable.value) << '\n';
    }

    output << "command: ";
    write_command(output, job.spec.argv);
    output << '\n';

    if (job.result) {
        if (job.result->exit_code) {
            output << "exit code: " << *job.result->exit_code << '\n';
        }
        if (job.result->terminating_signal) {
            output << "terminating signal: " << *job.result->terminating_signal
                   << '\n';
        }
        output << "dumped core: " << (job.result->dumped_core ? "yes" : "no")
               << '\n';
    } else {
        output << "result: not available\n";
    }

    return output.str();
}

std::string format_nodes(const std::vector<NodeSummary>& nodes) {
    std::ostringstream output;
    output << std::left << std::setw(16) << "node" << std::setw(12) << "state"
           << std::setw(22) << "cpus t/r/u/a" << std::setw(28)
           << "memory mb t/r/u/a" << "gpus t/r/u/a\n";

    for (const auto& node : nodes) {
        // each resource cell is total/reserved/used/available. keeping the
        // same order for all three is dense, but still less awful than fourteen
        // mostly-empty columns on a laptop terminal
        output << std::left << std::setw(16) << node.id << std::setw(12)
               << node_state_name(node.state) << std::setw(22)
               << resource_cell(node.total.cpus, node.reserved.cpus,
                                node.allocated.cpus, node.available.cpus)
               << std::setw(28)
               << resource_cell(node.total.memory_mb, node.reserved.memory_mb,
                                node.allocated.memory_mb,
                                node.available.memory_mb)
               << resource_cell(node.total.gpus, node.reserved.gpus,
                                node.allocated.gpus, node.available.gpus)
               << '\n';
    }

    return output.str();
}

std::string_view queue_usage() {
    return R"usage(usage: rlbs queue [options]

options:
  --socket PATH          daemon socket (default: /tmp/rlbs.sock)
  -h, --help             show this help
)usage";
}

std::string_view status_usage() {
    return R"usage(usage: rlbs status [options] JOB_ID

options:
  --socket PATH          daemon socket (default: /tmp/rlbs.sock)
  -h, --help             show this help
)usage";
}

std::string_view cancel_usage() {
    return R"usage(usage: rlbs cancel [options] JOB_ID

options:
  --socket PATH          daemon socket (default: /tmp/rlbs.sock)
  -h, --help             show this help
)usage";
}

std::string_view nodes_usage() {
    return R"usage(usage: rlbs nodes [options]

resource columns use total/reserved/used/available.

options:
  --socket PATH          daemon socket (default: /tmp/rlbs.sock)
  -h, --help             show this help
)usage";
}

} // namespace rlbs
