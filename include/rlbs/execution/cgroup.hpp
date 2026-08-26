// cgroup v2 is the kernel-side resource fence. the scheduler's counters decide
// whether a job may start; this file makes that reservation real for the job.
#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <utility>

#include <sys/types.h>

#include <rlbs/core/resources.hpp>
#include <rlbs/core/types.hpp>

namespace rlbs {

enum class CgroupOperation { locate, create, configure, attach, inspect, remove };

struct CgroupError {
    CgroupOperation operation{CgroupOperation::locate};
    int system_error{0};
    std::string message;
};

class JobCgroup {
  public:
    JobCgroup(const JobCgroup&) = delete;
    JobCgroup& operator=(const JobCgroup&) = delete;
    JobCgroup(JobCgroup&&) noexcept = default;
    JobCgroup& operator=(JobCgroup&&) noexcept = default;
    ~JobCgroup() = default;

    [[nodiscard]] static std::expected<JobCgroup, CgroupError>
    create(const std::filesystem::path& root, JobId job_id,
           const ResourceRequest& request);

    [[nodiscard]] std::expected<void, CgroupError> attach(pid_t pid) const;
    [[nodiscard]] std::expected<bool, CgroupError> oom_killed() const;
    [[nodiscard]] std::expected<void, CgroupError> remove();

  private:
    explicit JobCgroup(std::filesystem::path path) : path_{std::move(path)} {}
    std::filesystem::path path_;
};

} // namespace rlbs
