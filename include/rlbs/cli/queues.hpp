// singular `rlbs queue` already lists jobs. this file owns the deliberately
// plural admin surface so job inspection and queue policy do not become one
// giant parser full of nearly identical words.
#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rlbs/core/batch_queue.hpp>

namespace rlbs {

enum class QueuesCommandAction {
    list,
    add,
    start,
    stop,
    enable,
    disable,
};

struct QueuesCommand {
    std::filesystem::path socket_path{"/tmp/rlbs.sock"};
    QueuesCommandAction action{QueuesCommandAction::list};
    std::string name;
    int priority{0};
    std::optional<std::uint32_t> max_running;
    bool show_help{false};
};

[[nodiscard]] std::expected<QueuesCommand, std::string>
parse_queues_command(std::span<const std::string_view> arguments);

[[nodiscard]] std::string
format_queues(const std::vector<BatchQueue>& queues);

[[nodiscard]] std::string_view queues_usage();

} // namespace rlbs
