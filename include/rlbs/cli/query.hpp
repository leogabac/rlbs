#pragma once

#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rlbs/control/protocol.hpp>

namespace rlbs {

struct QueueCommand {
    std::filesystem::path socket_path{"/tmp/rlbs.sock"};
    bool show_help{false};
};

struct StatusCommand {
    std::filesystem::path socket_path{"/tmp/rlbs.sock"};
    JobId job_id{0};
    bool show_help{false};
};

[[nodiscard]] std::expected<QueueCommand, std::string>
parse_queue_command(std::span<const std::string_view> arguments);

[[nodiscard]] std::expected<StatusCommand, std::string>
parse_status_command(std::span<const std::string_view> arguments);

[[nodiscard]] std::string format_queue(const std::vector<JobSummary>& jobs);
[[nodiscard]] std::string format_status(const Job& job);
[[nodiscard]] std::string_view queue_usage();
[[nodiscard]] std::string_view status_usage();

} // namespace rlbs
