#pragma once

#include <chrono>
#include <expected>
#include <string>
#include <string_view>

namespace rlbs {

[[nodiscard]] std::expected<std::chrono::seconds, std::string>
parse_walltime(std::string_view value);

[[nodiscard]] std::string format_duration(std::chrono::seconds duration);

} // namespace rlbs
