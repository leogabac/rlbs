#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <rlbs/core/resources.hpp>

namespace rlbs {

struct EnvironmentVariable {
    std::string name;
    std::string value;
};

// this is the part of a job the user actually submitted. keeping it away from
// runtime state means queue updates cannot quietly rewrite what was requested
struct JobSpec {
    std::string name;
    ResourceRequest resources;
    std::vector<std::string> argv;
    std::filesystem::path working_directory;
    std::vector<EnvironmentVariable> environment;
    bool inherit_environment{true};
    std::optional<std::filesystem::path> stdout_path;
    std::optional<std::filesystem::path> stderr_path;
    bool append_output{false};
    std::optional<std::chrono::seconds> walltime;
};

} // namespace rlbs
