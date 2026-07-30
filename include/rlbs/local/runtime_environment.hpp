#pragma once

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <rlbs/core/job.hpp>

namespace rlbs {

enum class RuntimeEnvironmentOperation {
    inspect_temp_directory,
    create_node_file,
    write_node_file,
};

struct RuntimeEnvironmentError {
    RuntimeEnvironmentOperation operation{
        RuntimeEnvironmentOperation::create_node_file};
    int system_error{0};
    std::string message;
};

// this owns the little pbs node file for exactly as long as the local job is
// active. moving transfers cleanup; copying would make two destructors race to
// unlink the same file, which is the usual raai footgun wearing a new hat
class PreparedRuntimeEnvironment {
  public:
    [[nodiscard]] static std::expected<PreparedRuntimeEnvironment,
                                       RuntimeEnvironmentError>
    create(const Job& job, std::string_view node_id);

    PreparedRuntimeEnvironment(const PreparedRuntimeEnvironment&) = delete;
    PreparedRuntimeEnvironment&
    operator=(const PreparedRuntimeEnvironment&) = delete;
    PreparedRuntimeEnvironment(PreparedRuntimeEnvironment&& other) noexcept;
    PreparedRuntimeEnvironment&
    operator=(PreparedRuntimeEnvironment&&) = delete;
    ~PreparedRuntimeEnvironment();

    [[nodiscard]] const std::vector<EnvironmentVariable>& variables() const;
    [[nodiscard]] const std::filesystem::path& node_file() const;

  private:
    PreparedRuntimeEnvironment(std::vector<EnvironmentVariable> variables,
                               std::filesystem::path node_file);

    std::vector<EnvironmentVariable> variables_;
    std::filesystem::path node_file_;
    bool owns_node_file_{true};
};

} // namespace rlbs
