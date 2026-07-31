#include <csignal>
#include <cstring>
#include <iostream>
#include <sstream>
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
#include <rlbs/logging/logger.hpp>
#include <rlbs/persistence/database.hpp>
#include <rlbs/persistence/job_repository.hpp>
#include <rlbs/persistence/queue_repository.hpp>

namespace {

volatile std::sig_atomic_t stop_requested = 0;

void request_stop(int) {
    // signal handlers get a very tiny list of safe operations, so just flip
    // this flag and let the ordinary daemon loop do the actual shutdown work
    stop_requested = 1;
}

} // namespace

int main(int argc, char* argv[]) {
    rlbs::Logger logger{"rlbsd"};
    std::vector<std::string_view> arguments;
    arguments.reserve(argc > 0 ? static_cast<std::size_t>(argc - 1) : 0);

    for (int index = 1; index < argc; ++index) {
        arguments.emplace_back(argv[index]);
    }

    auto config = rlbs::parse_daemon_config(arguments);

    if (!config) {
        logger.error("config", config.error());
        std::cerr << rlbs::daemon_usage();
        return 2;
    }

    if (config->show_help) {
        std::cout << rlbs::daemon_usage();
        return 0;
    }

    auto database = rlbs::SqliteDatabase::open(config->database_path);

    if (!database) {
        logger.error("database", "could not open " +
                                     config->database_path.string() + ": " +
                                     database.error().message);
        return 1;
    }

    rlbs::JobRepository repository{*database};
    rlbs::QueueRepository queues{*database};
    rlbs::FirstFitScheduler scheduler;
    rlbs::Node local_node{config->node_id, config->capacity, config->reserved};
    rlbs::LocalCoordinator coordinator{
        repository,
        queues,
        std::move(local_node),
        scheduler,
        config->spool_path,
        &logger,
    };
    auto control = rlbs::ControlServer::listen(config->socket_path, repository,
                                               coordinator, &logger);

    if (!control) {
        std::string message = "could not open " + config->socket_path.string() +
                              ": " + control.error().message;

        if (control.error().system_error != 0) {
            message += ": ";
            message += std::strerror(control.error().system_error);
        }

        logger.error("control", message);
        return 1;
    }

    std::signal(SIGINT, request_stop);
    std::signal(SIGTERM, request_stop);

    std::ostringstream startup;
    startup << "started version=" << rlbs::version()
            << " node=" << config->node_id
            << " database=" << config->database_path
            << " socket=" << config->socket_path
            << " spool=" << config->spool_path
            << " cpus=" << config->capacity.cpus
            << " memory_mb=" << config->capacity.memory_mb
            << " gpus=" << config->capacity.gpus
            << " reserved_cpus=" << config->reserved.cpus
            << " reserved_memory_mb=" << config->reserved.memory_mb
            << " reserved_gpus=" << config->reserved.gpus;
    logger.info("daemon", startup.str());

    while (stop_requested == 0) {
        auto handled = control->poll();

        if (!handled) {
            logger.error("control",
                         "socket poll failed: " + handled.error().message);
            return 1;
        }

        auto ticked = coordinator.tick();

        if (!ticked) {
            logger.error("scheduler", "tick failed: " + ticked.error().message);
            return 1;
        }

        std::this_thread::sleep_for(config->tick_interval);
    }

    // stop accepting new submissions before draining active jobs. otherwise a
    // client could sneak more work in after shutdown already started
    control->close();
    logger.info("daemon", "shutdown requested active_jobs=" +
                              std::to_string(coordinator.active_job_count()));

    // once shutdown starts, do not pull another job from the queue. already
    // running work still gets polled until it exits so we do not orphan it
    while (coordinator.active_job_count() > 0) {
        auto ticked = coordinator.tick(false);

        if (!ticked) {
            logger.error("scheduler",
                         "shutdown poll failed: " + ticked.error().message);
            return 1;
        }

        std::this_thread::sleep_for(config->tick_interval);
    }

    logger.info("daemon", "stopped");
    return 0;
}
