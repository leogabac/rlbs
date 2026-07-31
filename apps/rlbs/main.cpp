#include <cerrno>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include <rlbs/cli/query.hpp>
#include <rlbs/cli/submit.hpp>
#include <rlbs/control/unix_socket.hpp>
#include <rlbs/core/version.hpp>

namespace {

enum class CommandStyle {
    native,
    pbs,
};

[[nodiscard]] std::vector<std::string_view> command_arguments(int argc,
                                                              char* argv[]) {
    std::vector<std::string_view> arguments;
    arguments.reserve(argc > 0 ? static_cast<std::size_t>(argc) : 0);

    for (int index = 0; index < argc; ++index) {
        arguments.emplace_back(argv[index]);
    }

    return arguments;
}

[[nodiscard]] std::string_view executable_name(std::string_view path) {
    const auto separator = path.find_last_of('/');
    return separator == std::string_view::npos ? path
                                               : path.substr(separator + 1);
}

[[nodiscard]] std::string command_name(std::string_view native_name,
                                       CommandStyle style) {
    if (style == CommandStyle::pbs) {
        if (native_name == "submit") {
            return "qsub";
        }
        if (native_name == "queue" || native_name == "status") {
            return "qstat";
        }
        if (native_name == "cancel") {
            return "qdel";
        }
    }

    return "rlbs " + std::string{native_name};
}

[[nodiscard]] std::string_view qsub_usage() {
    return R"usage(usage: qsub [--socket PATH] [-q QUEUE] JOB.pbs

the supported #PBS directives are -N, -q, -l, -d, -V, -v, -o, and -e.
walltime uses -l walltime=HH:MM:SS.
)usage";
}

[[nodiscard]] std::string_view qstat_usage() {
    return R"usage(usage: qstat [--socket PATH] [JOB_ID]

without a job id qstat lists the queue. with one it shows that job.
)usage";
}

[[nodiscard]] std::string_view qdel_usage() {
    return R"usage(usage: qdel [--socket PATH] JOB_ID
)usage";
}

void print_control_error(std::string_view command,
                         const rlbs::ControlSocketError& error) {
    std::cerr << command << ": " << error.message;

    if (error.system_error != 0) {
        std::cerr << ": " << std::strerror(error.system_error);
    }

    std::cerr << '\n';
}

int submit(int argc, char* argv[], CommandStyle style) {
    auto command = rlbs::parse_submit_command(command_arguments(argc, argv));
    const auto name = command_name("submit", style);
    const auto usage =
        style == CommandStyle::pbs ? qsub_usage() : rlbs::submit_usage();

    if (!command) {
        std::cerr << name << ": " << command.error() << '\n' << usage;
        return 2;
    }

    if (command->show_help) {
        std::cout << usage;
        return 0;
    }

    rlbs::ControlClient client{command->socket_path};
    auto job_id = client.submit(command->spec);

    if (!job_id) {
        print_control_error(name, job_id.error());
        return 1;
    }

    if (style == CommandStyle::pbs) {
        // pbs scripts tend to capture this value, so extra friendly prose here
        // just turns into annoying string cleanup in every launcher
        std::cout << *job_id << '\n';
    } else {
        std::cout << "submitted job " << *job_id << '\n';
    }
    return 0;
}

int queue(int argc, char* argv[], CommandStyle style) {
    auto command = rlbs::parse_queue_command(command_arguments(argc, argv));
    const auto name = command_name("queue", style);
    const auto usage =
        style == CommandStyle::pbs ? qstat_usage() : rlbs::queue_usage();

    if (!command) {
        std::cerr << name << ": " << command.error() << '\n' << usage;
        return 2;
    }
    if (command->show_help) {
        std::cout << usage;
        return 0;
    }

    rlbs::ControlClient client{command->socket_path};
    auto jobs = client.queue();

    if (!jobs) {
        print_control_error(name, jobs.error());
        return 1;
    }

    std::cout << rlbs::format_queue(*jobs);
    return 0;
}

int status(int argc, char* argv[], CommandStyle style) {
    auto command = rlbs::parse_status_command(command_arguments(argc, argv));
    const auto name = command_name("status", style);
    const auto usage =
        style == CommandStyle::pbs ? qstat_usage() : rlbs::status_usage();

    if (!command) {
        std::cerr << name << ": " << command.error() << '\n' << usage;
        return 2;
    }
    if (command->show_help) {
        std::cout << usage;
        return 0;
    }

    rlbs::ControlClient client{command->socket_path};
    auto job = client.status(command->job_id);

    if (!job) {
        print_control_error(name, job.error());
        return 1;
    }

    std::cout << rlbs::format_status(*job);
    return 0;
}

int cancel(int argc, char* argv[], CommandStyle style) {
    auto command = rlbs::parse_cancel_command(command_arguments(argc, argv));
    const auto name = command_name("cancel", style);
    const auto usage =
        style == CommandStyle::pbs ? qdel_usage() : rlbs::cancel_usage();

    if (!command) {
        std::cerr << name << ": " << command.error() << '\n' << usage;
        return 2;
    }
    if (command->show_help) {
        std::cout << usage;
        return 0;
    }

    rlbs::ControlClient client{command->socket_path};
    auto job_id = client.cancel(command->job_id);

    if (!job_id) {
        print_control_error(name, job_id.error());
        return 1;
    }

    if (style == CommandStyle::native) {
        std::cout << "cancellation requested for job " << *job_id << '\n';
    }
    return 0;
}

int nodes(int argc, char* argv[]) {
    auto command = rlbs::parse_nodes_command(command_arguments(argc, argv));

    if (!command) {
        std::cerr << "rlbs nodes: " << command.error() << '\n'
                  << rlbs::nodes_usage();
        return 2;
    }
    if (command->show_help) {
        std::cout << rlbs::nodes_usage();
        return 0;
    }

    rlbs::ControlClient client{command->socket_path};
    auto node_list = client.nodes();

    if (!node_list) {
        print_control_error("rlbs nodes", node_list.error());
        return 1;
    }

    std::cout << rlbs::format_nodes(*node_list);
    return 0;
}

[[nodiscard]] bool qstat_has_job_id(int argc, char* argv[]) {
    // qstat owns both queue and status. skip our socket extension and its value,
    // then any argument left that is not help has to be the requested job id
    for (int index = 0; index < argc; ++index) {
        const std::string_view argument{argv[index]};

        if (argument == "--socket") {
            ++index;
            continue;
        }
        if (argument != "--help" && argument != "-h") {
            return true;
        }
    }

    return false;
}

} // namespace

int main(int argc, char* argv[]) {
    const auto invoked_as =
        executable_name(argc > 0 ? std::string_view{argv[0]} : "rlbs");

    // these are aliases in the literal filesystem sense. once they land here
    // they use the same parsers, socket client, and handlers as the native cli
    if (invoked_as == "qsub") {
        return submit(argc - 1, argv + 1, CommandStyle::pbs);
    }
    if (invoked_as == "qdel") {
        return cancel(argc - 1, argv + 1, CommandStyle::pbs);
    }
    if (invoked_as == "qstat") {
        if (qstat_has_job_id(argc - 1, argv + 1)) {
            return status(argc - 1, argv + 1, CommandStyle::pbs);
        }
        return queue(argc - 1, argv + 1, CommandStyle::pbs);
    }

    if (argc < 2) {
        std::cout << rlbs::cli_usage();
        return 0;
    }

    const std::string_view command{argv[1]};

    if (command == "--version") {
        std::cout << rlbs::project_name() << " " << rlbs::version() << '\n';
        return 0;
    }

    if (command == "--help" || command == "-h") {
        std::cout << rlbs::cli_usage();
        return 0;
    }

    if (command == "submit") {
        return submit(argc - 2, argv + 2, CommandStyle::native);
    }
    if (command == "queue") {
        return queue(argc - 2, argv + 2, CommandStyle::native);
    }
    if (command == "status") {
        return status(argc - 2, argv + 2, CommandStyle::native);
    }
    if (command == "cancel") {
        return cancel(argc - 2, argv + 2, CommandStyle::native);
    }
    if (command == "nodes") {
        return nodes(argc - 2, argv + 2);
    }

    std::cerr << "rlbs: unknown command: " << command << '\n'
              << rlbs::cli_usage();
    return 2;
}
