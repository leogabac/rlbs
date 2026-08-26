#include <rlbs/execution/cgroup.hpp>

#include <cerrno>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <unistd.h>

namespace rlbs {
namespace {

[[nodiscard]] CgroupError error(CgroupOperation operation, int system_error,
                                std::string message) {
    return {.operation = operation,
            .system_error = system_error,
            .message = std::move(message)};
}

[[nodiscard]] std::expected<void, CgroupError>
write_value(const std::filesystem::path& path, std::string value,
            CgroupOperation operation) {
    std::ofstream output{path};
    if (!output) {
        return std::unexpected{error(operation, errno, path.string())};
    }
    output << value;
    if (!output) {
        return std::unexpected{error(operation, EIO, path.string())};
    }
    return {};
}

[[nodiscard]] std::expected<std::uint64_t, CgroupError>
read_counter(const std::filesystem::path& path, std::string_view key) {
    std::ifstream input{path};
    if (!input) {
        return std::unexpected{error(CgroupOperation::inspect, errno,
                                     path.string())};
    }
    std::string name;
    std::uint64_t value = 0;
    while (input >> name >> value) {
        if (name == key) {
            return value;
        }
    }
    return std::unexpected{error(CgroupOperation::inspect, EPROTO,
                                 "missing cgroup event " + std::string{key})};
}

[[nodiscard]] std::expected<std::filesystem::path, CgroupError>
own_cgroup_path(const std::filesystem::path& root) {
    std::ifstream input{"/proc/self/cgroup"};
    if (!input) {
        return std::unexpected{error(CgroupOperation::locate, errno,
                                     "/proc/self/cgroup")};
    }
    std::string line;
    while (std::getline(input, line)) {
        if (line.starts_with("0::")) {
            return root / line.substr(3).substr(1);
        }
    }
    return std::unexpected{error(CgroupOperation::locate, EPROTO,
                                 "cgroup v2 is not active")};
}

} // namespace

std::expected<JobCgroup, CgroupError>
JobCgroup::create(const std::filesystem::path& root, JobId job_id,
                  const ResourceRequest& request) {
    auto parent = own_cgroup_path(root);
    if (!parent) {
        return std::unexpected{std::move(parent.error())};
    }

    const auto path = *parent / ("rlbs-job-" + std::to_string(job_id));
    std::error_code filesystem_error;
    const bool created =
        std::filesystem::create_directory(path, filesystem_error);
    if (filesystem_error || !created) {
        return std::unexpected{error(CgroupOperation::create,
                                     filesystem_error ? filesystem_error.value()
                                                       : EEXIST,
                                     path.string())};
    }

    const auto fail = [&](CgroupError failure)
        -> std::expected<JobCgroup, CgroupError> {
        std::filesystem::remove(path, filesystem_error);
        return std::unexpected{std::move(failure)};
    };

    const std::uint64_t bytes_per_mb = 1024 * 1024;
    if (request.memory_mb > std::numeric_limits<std::uint64_t>::max() /
                                bytes_per_mb) {
        return fail(error(CgroupOperation::configure, EOVERFLOW,
                          "memory request is too large"));
    }
    const auto memory = request.memory_mb == 0
                            ? std::string{"max"}
                            : std::to_string(request.memory_mb * bytes_per_mb);
    if (auto result = write_value(path / "memory.max", memory,
                                  CgroupOperation::configure);
        !result) {
        return fail(std::move(result.error()));
    }
    if (auto result = write_value(path / "memory.swap.max", "0",
                                  CgroupOperation::configure);
        !result) {
        return fail(std::move(result.error()));
    }

    constexpr std::uint64_t period = 100000;
    if (request.cpus == 0 ||
        request.cpus > std::numeric_limits<std::uint64_t>::max() / period) {
        return fail(error(CgroupOperation::configure, EINVAL,
                          "invalid cpu request"));
    }
    if (auto result = write_value(
            path / "cpu.max", std::to_string(request.cpus * period) +
                                  " " + std::to_string(period),
            CgroupOperation::configure);
        !result) {
        return fail(std::move(result.error()));
    }

    return JobCgroup{path};
}

std::expected<void, CgroupError> JobCgroup::attach(pid_t pid) const {
    return write_value(path_ / "cgroup.procs", std::to_string(pid),
                       CgroupOperation::attach);
}

std::expected<bool, CgroupError> JobCgroup::oom_killed() const {
    auto counter = read_counter(path_ / "memory.events", "oom_kill");
    if (!counter) {
        return std::unexpected{std::move(counter.error())};
    }
    return *counter != 0;
}

std::expected<void, CgroupError> JobCgroup::remove() {
    std::error_code filesystem_error;
    if (!std::filesystem::remove(path_, filesystem_error) &&
        filesystem_error) {
        return std::unexpected{error(CgroupOperation::remove,
                                     filesystem_error.value(), path_.string())};
    }
    path_.clear();
    return {};
}

} // namespace rlbs
