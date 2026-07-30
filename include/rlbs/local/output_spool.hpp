#pragma once

#include <expected>
#include <filesystem>
#include <string>

#include <rlbs/core/job.hpp>

namespace rlbs {

enum class OutputSpoolOperation {
    create_directory,
    create_stage_file,
    read_output,
    write_output,
    publish_output,
    clean_spool,
};

struct OutputSpoolError {
    OutputSpoolOperation operation{OutputSpoolOperation::create_directory};
    int system_error{0};
    std::string message;
};

// this is the execution-node side of output handling. the runner writes only
// here; stage() is the one place that knows where the user wanted the files
class PreparedOutputSpool {
  public:
    [[nodiscard]] static std::expected<PreparedOutputSpool, OutputSpoolError>
    create(const std::filesystem::path& spool_root, const Job& job);

    PreparedOutputSpool(const PreparedOutputSpool&) = delete;
    PreparedOutputSpool& operator=(const PreparedOutputSpool&) = delete;
    PreparedOutputSpool(PreparedOutputSpool&&) noexcept = default;
    PreparedOutputSpool& operator=(PreparedOutputSpool&&) noexcept = default;

    [[nodiscard]] const std::filesystem::path& stdout_path() const;
    [[nodiscard]] const std::filesystem::path& stderr_path() const;
    [[nodiscard]] const std::filesystem::path& directory() const;

    [[nodiscard]] std::expected<void, OutputSpoolError> stage();

  private:
    PreparedOutputSpool(std::filesystem::path directory,
                        std::filesystem::path stdout_spool,
                        std::filesystem::path stderr_spool,
                        std::filesystem::path stdout_destination,
                        std::filesystem::path stderr_destination,
                        bool append_output, bool shared_output);

    [[nodiscard]] std::expected<void, OutputSpoolError>
    stage_one(const std::filesystem::path& source,
              const std::filesystem::path& destination);

    std::filesystem::path directory_;
    std::filesystem::path stdout_spool_;
    std::filesystem::path stderr_spool_;
    std::filesystem::path stdout_destination_;
    std::filesystem::path stderr_destination_;
    bool append_output_{false};
    bool shared_output_{false};
    bool stdout_staged_{false};
    bool stderr_staged_{false};
};

} // namespace rlbs
