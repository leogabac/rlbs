#include <cerrno>
#include <cstring>
#include <iostream>
#include <string_view>
#include <vector>

#include <rlbs/cli/submit.hpp>
#include <rlbs/control/unix_socket.hpp>
#include <rlbs/core/version.hpp>

namespace {

int submit(int argc, char* argv[]) {
    std::vector<std::string_view> arguments;
    arguments.reserve(argc > 0 ? static_cast<std::size_t>(argc) : 0);

    for (int index = 0; index < argc; ++index) {
        arguments.emplace_back(argv[index]);
    }

    auto command = rlbs::parse_submit_command(arguments);

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
        std::cerr << "rlbs submit: " << job_id.error().message;

        if (job_id.error().system_error != 0) {
            std::cerr << ": " << std::strerror(job_id.error().system_error);
        }

        std::cerr << '\n';
        return 1;
    }

    std::cout << "submitted job " << *job_id << '\n';
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

    std::cerr << "rlbs: unknown command: " << command << '\n'
              << rlbs::cli_usage();
    return 2;
}
