#include <csignal>
#include <cstring>
#include <iostream>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <rlbs/control/unix_socket.hpp>
#include <rlbs/core/node.hpp>
#include <rlbs/core/scheduler.hpp>
#include <rlbs/core/version.hpp>
#include <rlbs/daemon/config.hpp>
#include <rlbs/local/coordinator.hpp>
#include <rlbs/persistence/database.hpp>
#include <rlbs/persistence/job_repository.hpp>

namespace {

volatile std::sig_atomic_t stop_requested = 0;

void request_stop(int) {
    // signal handlers get a very tiny list of safe operations, so just flip
    // this flag and let the ordinary daemon loop do the actual shutdown work
    stop_requested = 1;
}

} // namespace

int main(int argc, char* argv[]) {
    std::vector<std::string_view> arguments;
    arguments.reserve(argc > 0 ? static_cast<std::size_t>(argc - 1) : 0);

    for (int index = 1; index < argc; ++index) {
        arguments.emplace_back(argv[index]);
    }

    auto config = rlbs::parse_daemon_config(arguments);

    if (!config) {
        std::cerr << "rlbsd: " << config.error() << '\n'
                  << rlbs::daemon_usage();
        return 2;
    }

    if (config->show_help) {
        std::cout << rlbs::daemon_usage();
        return 0;
    }

    auto database = rlbs::SqliteDatabase::open(config->database_path);

    if (!database) {
        std::cerr << "rlbsd: could not open database: "
                  << database.error().message << '\n';
        return 1;
    }

    rlbs::JobRepository repository{*database};
    auto control = rlbs::ControlServer::listen(config->socket_path, repository);

    if (!control) {
        std::cerr << "rlbsd: could not open control socket: "
                  << control.error().message;

        if (control.error().system_error != 0) {
            std::cerr << ": " << std::strerror(control.error().system_error);
        }

        std::cerr << '\n';
        return 1;
    }

    rlbs::FirstFitScheduler scheduler;
    rlbs::Node local_node{config->node_id, config->capacity, config->reserved};
    rlbs::LocalCoordinator coordinator{
        repository,
        std::move(local_node),
        scheduler,
    };

    std::signal(SIGINT, request_stop);
    std::signal(SIGTERM, request_stop);

    std::cout << "starting " << rlbs::project_name() << " daemon "
              << rlbs::version() << " on node " << config->node_id << " using "
              << config->socket_path << '\n';

    while (stop_requested == 0) {
        auto handled = control->poll();

        if (!handled) {
            std::cerr << "rlbsd: control socket failed: "
                      << handled.error().message << '\n';
            return 1;
        }

        auto ticked = coordinator.tick();

        if (!ticked) {
            std::cerr << "rlbsd: scheduler tick failed: "
                      << ticked.error().message << '\n';
            return 1;
        }

        std::this_thread::sleep_for(config->tick_interval);
    }

    // stop accepting new submissions before draining active jobs. otherwise a
    // client could sneak more work in after shutdown already started
    control->close();

    // once shutdown starts, do not pull another job from the queue. already
    // running work still gets polled until it exits so we do not orphan it
    while (coordinator.active_job_count() > 0) {
        auto ticked = coordinator.tick(false);

        if (!ticked) {
            std::cerr << "rlbsd: shutdown poll failed: "
                      << ticked.error().message << '\n';
            return 1;
        }

        std::this_thread::sleep_for(config->tick_interval);
    }

    return 0;
}
