// daemon config is also the boundary between friendly names from argv and the
// numeric ids needed by unix syscalls. keep that lookup explicit and testable.
#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <rlbs/core/resources.hpp>
#include <rlbs/core/types.hpp>
#include <rlbs/control/defaults.hpp>

namespace rlbs {

struct DaemonConfig {
    std::filesystem::path database_path{"rlbs.db"};
    std::filesystem::path socket_path{default_control_socket};
    std::optional<std::string> socket_group;
    std::optional<std::filesystem::path> cgroup_root;
    std::filesystem::path spool_path;
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

// group names are resolved at daemon startup, not while parsing options. that
// keeps config parsing deterministic and gives nss failures their own message.
[[nodiscard]] std::expected<std::uint32_t, std::string>
resolve_socket_group(std::string_view name);

[[nodiscard]] std::string_view daemon_usage();

} // namespace rlbs
