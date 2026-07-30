#pragma once

#include <expected>
#include <filesystem>
#include <string>

#include <rlbs/core/job_spec.hpp>

namespace rlbs {

[[nodiscard]] std::expected<JobSpec, std::string>
parse_native_script(const std::filesystem::path& script_path,
                    const std::filesystem::path& submission_directory);

[[nodiscard]] bool
is_native_script_path(const std::filesystem::path& script_path);

} // namespace rlbs
