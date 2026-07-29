#pragma once

#include <chrono>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

#include <rlbs/core/resources.hpp>
#include <rlbs/core/types.hpp>

namespace rlbs {

struct DaemonConfig {
    std::filesystem::path database_path{"rlbs.db"};
    std::filesystem::path socket_path{"/tmp/rlbs.sock"};
    NodeId node_id{"local"};
    ResourceCapacity capacity{
        .cpus = 1,
        .memory_mb = 0,
        .gpus = 0,
    };
    ResourceCapacity reserved{
        .cpus = 0,
        .memory_mb = 0,
        .gpus = 0,
    };
    std::chrono::milliseconds tick_interval{100};
    bool show_help{false};
};

[[nodiscard]] std::expected<DaemonConfig, std::string>
parse_daemon_config(std::span<const std::string_view> arguments);

[[nodiscard]] std::string_view daemon_usage();

} // namespace rlbs
