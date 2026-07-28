#pragma once

#include <string_view>

namespace rlbs {
[[nodiscard]] std::string_view version();
[[nodiscard]] std::string_view project_name();
} // namespace rlbs
