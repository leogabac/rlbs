#pragma once

#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

#include <rlbs/core/job_spec.hpp>

namespace rlbs {

struct SubmitCommand {
    std::filesystem::path socket_path{"/tmp/rlbs.sock"};
    JobSpec spec;
    bool show_help{false};
};

[[nodiscard]] std::expected<SubmitCommand, std::string>
parse_submit_command(std::span<const std::string_view> arguments);

[[nodiscard]] std::string_view submit_usage();
[[nodiscard]] std::string_view cli_usage();

} // namespace rlbs
