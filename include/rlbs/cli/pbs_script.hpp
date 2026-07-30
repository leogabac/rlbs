#pragma once

#include <expected>
#include <filesystem>
#include <span>
#include <string>

#include <rlbs/core/job_spec.hpp>

namespace rlbs {

[[nodiscard]] std::expected<JobSpec, std::string>
parse_pbs_script(const std::filesystem::path& script_path,
                 const std::filesystem::path& submission_directory,
                 std::span<const EnvironmentVariable> submission_environment);

} // namespace rlbs
