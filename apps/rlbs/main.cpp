#include <cerrno>
#include <cstring>
#include <iostream>
#include <string_view>
#include <vector>

#include <rlbs/cli/query.hpp>
#include <rlbs/cli/submit.hpp>
#include <rlbs/control/unix_socket.hpp>
#include <rlbs/core/version.hpp>

namespace {

[[nodiscard]] std::vector<std::string_view> command_arguments(int argc,
                                                              char* argv[]) {
    std::vector<std::string_view> arguments;
    arguments.reserve(argc > 0 ? static_cast<std::size_t>(argc) : 0);

    for (int index = 0; index < argc; ++index) {
        arguments.emplace_back(argv[index]);
    }

    return arguments;
}

void print_control_error(std::string_view command,
                         const rlbs::ControlSocketError& error) {
    std::cerr << "rlbs " << command << ": " << error.message;

    if (error.system_error != 0) {
        std::cerr << ": " << std::strerror(error.system_error);
    }

    std::cerr << '\n';
}

int submit(int argc, char* argv[]) {
    auto command = rlbs::parse_submit_command(command_arguments(argc, argv));

    if (!command) {
        std::cerr << "rlbs submit: " << command.error() << '\n'
                  << rlbs::submit_usage();
        return 2;
    }

    if (command->show_help) {
        std::cout << rlbs::submit_usage();
        return 0;
    }

    rlbs::ControlClient client{command->socket_path};
    auto job_id = client.submit(command->spec);

    if (!job_id) {
        print_control_error("submit", job_id.error());
        return 1;
    }

    std::cout << "submitted job " << *job_id << '\n';
    return 0;
}

int queue(int argc, char* argv[]) {
    auto command = rlbs::parse_queue_command(command_arguments(argc, argv));

    if (!command) {
        std::cerr << "rlbs queue: " << command.error() << '\n'
                  << rlbs::queue_usage();
        return 2;
    }
    if (command->show_help) {
        std::cout << rlbs::queue_usage();
        return 0;
    }

    rlbs::ControlClient client{command->socket_path};
    auto jobs = client.queue();

    if (!jobs) {
        print_control_error("queue", jobs.error());
        return 1;
    }

    std::cout << rlbs::format_queue(*jobs);
    return 0;
}

int status(int argc, char* argv[]) {
    auto command = rlbs::parse_status_command(command_arguments(argc, argv));

    if (!command) {
        std::cerr << "rlbs status: " << command.error() << '\n'
                  << rlbs::status_usage();
        return 2;
    }
    if (command->show_help) {
        std::cout << rlbs::status_usage();
        return 0;
    }

    rlbs::ControlClient client{command->socket_path};
    auto job = client.status(command->job_id);

    if (!job) {
        print_control_error("status", job.error());
        return 1;
    }

    std::cout << rlbs::format_status(*job);
    return 0;
}

int cancel(int argc, char* argv[]) {
    auto command = rlbs::parse_cancel_command(command_arguments(argc, argv));

    if (!command) {
        std::cerr << "rlbs cancel: " << command.error() << '\n'
                  << rlbs::cancel_usage();
        return 2;
    }
    if (command->show_help) {
        std::cout << rlbs::cancel_usage();
        return 0;
    }

    rlbs::ControlClient client{command->socket_path};
    auto job_id = client.cancel(command->job_id);

    if (!job_id) {
        print_control_error("cancel", job_id.error());
        return 1;
    }

    std::cout << "cancellation requested for job " << *job_id << '\n';
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
        print_control_error("nodes", node_list.error());
        return 1;
    }

    std::cout << rlbs::format_nodes(*node_list);
    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
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
        return submit(argc - 2, argv + 2);
    }
    if (command == "queue") {
        return queue(argc - 2, argv + 2);
    }
    if (command == "status") {
        return status(argc - 2, argv + 2);
    }
    if (command == "cancel") {
        return cancel(argc - 2, argv + 2);
    }
    if (command == "nodes") {
        return nodes(argc - 2, argv + 2);
    }

    std::cerr << "rlbs: unknown command: " << command << '\n'
              << rlbs::cli_usage();
    return 2;
}
