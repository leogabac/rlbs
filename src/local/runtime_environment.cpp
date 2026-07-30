#include <rlbs/local/runtime_environment.hpp>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <system_error>
#include <utility>

#include <unistd.h>

namespace rlbs {
namespace {

class UniqueFd {
  public:
    explicit UniqueFd(int fd) : fd_{fd} {}

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    ~UniqueFd() {
        if (fd_ >= 0) {
            static_cast<void>(::close(fd_));
        }
    }

    [[nodiscard]] int get() const { return fd_; }

  private:
    int fd_{-1};
};

[[nodiscard]] RuntimeEnvironmentError
error(RuntimeEnvironmentOperation operation, int system_error,
      std::string message) {
    return {
        .operation = operation,
        .system_error = system_error,
        .message = std::move(message),
    };
}

void set_variable(std::vector<EnvironmentVariable>& variables, std::string name,
                  std::string value) {
    const auto existing =
        std::ranges::find(variables, name, &EnvironmentVariable::name);

    // pbs values describe what the scheduler actually did. a submitted
    // PBS_JOBID=fake cannot be allowed to override reality for the child
    if (existing != variables.end()) {
        existing->value = std::move(value);
        return;
    }

    variables.push_back({
        .name = std::move(name),
        .value = std::move(value),
    });
}

[[nodiscard]] std::expected<std::filesystem::path, RuntimeEnvironmentError>
create_node_file(const Job& job, std::string_view node_id) {
    if (node_id.empty() || node_id.contains('\n') || node_id.contains('\r') ||
        node_id.contains('\0')) {
        return std::unexpected{
            error(RuntimeEnvironmentOperation::create_node_file, EINVAL,
                  "local node id cannot be written to a pbs node file")};
    }

    std::error_code filesystem_error;
    const auto temporary =
        std::filesystem::temp_directory_path(filesystem_error);

    if (filesystem_error) {
        return std::unexpected{
            error(RuntimeEnvironmentOperation::inspect_temp_directory,
                  filesystem_error.value(),
                  "could not find the temporary directory")};
    }

    // mkstemp replaces the xxxxxx suffix atomically and opens mode 0600. job
    // ids repeat across databases, so relying on just the id would eventually
    // make two daemons overwrite each other's node file. obviously delightful
    auto pattern =
        (temporary / ("rlbs-job-" + std::to_string(job.id) + "-XXXXXX"))
            .string();
    std::vector<char> writable_pattern(pattern.begin(), pattern.end());
    writable_pattern.push_back('\0');

    UniqueFd file{::mkstemp(writable_pattern.data())};

    if (file.get() < 0) {
        return std::unexpected{
            error(RuntimeEnvironmentOperation::create_node_file, errno,
                  "could not create the pbs node file")};
    }

    std::string contents;
    contents.reserve((node_id.size() + 1) * job.spec.resources.cpus);

    // pbs node files contain one hostname per allocated cpu slot. local mode
    // has one node, so its name repeats rather than inventing fake machines
    for (std::uint32_t cpu = 0; cpu < job.spec.resources.cpus; ++cpu) {
        contents.append(node_id);
        contents.push_back('\n');
    }

    std::size_t written = 0;

    while (written < contents.size()) {
        const auto result = ::write(file.get(), contents.data() + written,
                                    contents.size() - written);

        if (result > 0) {
            written += static_cast<std::size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }

        const int write_error = result < 0 ? errno : EIO;
        static_cast<void>(::unlink(writable_pattern.data()));
        return std::unexpected{
            error(RuntimeEnvironmentOperation::write_node_file, write_error,
                  "could not write the pbs node file")};
    }

    return std::filesystem::path{writable_pattern.data()};
}

} // namespace

std::expected<PreparedRuntimeEnvironment, RuntimeEnvironmentError>
PreparedRuntimeEnvironment::create(const Job& job, std::string_view node_id) {
    auto node_file = create_node_file(job, node_id);

    if (!node_file) {
        return std::unexpected{std::move(node_file.error())};
    }

    auto variables = job.spec.environment;
    set_variable(variables, "PBS_JOBID", std::to_string(job.id));
    set_variable(variables, "PBS_JOBNAME", job.spec.name);
    set_variable(variables, "PBS_O_WORKDIR",
                 job.spec.working_directory.string());
    set_variable(variables, "PBS_NODEFILE", node_file->string());

    return PreparedRuntimeEnvironment{std::move(variables),
                                      std::move(*node_file)};
}

PreparedRuntimeEnvironment::PreparedRuntimeEnvironment(
    std::vector<EnvironmentVariable> variables, std::filesystem::path node_file)
    : variables_{std::move(variables)}, node_file_{std::move(node_file)} {}

PreparedRuntimeEnvironment::PreparedRuntimeEnvironment(
    PreparedRuntimeEnvironment&& other) noexcept
    : variables_{std::move(other.variables_)},
      node_file_{std::move(other.node_file_)},
      owns_node_file_{std::exchange(other.owns_node_file_, false)} {}

PreparedRuntimeEnvironment::~PreparedRuntimeEnvironment() {
    if (owns_node_file_) {
        // the process is already reaped before this object dies. unlink is
        // best-effort because a user deleting their own node file is not a
        // reason to turn a completed job into a daemon failure
        static_cast<void>(::unlink(node_file_.c_str()));
    }
}

const std::vector<EnvironmentVariable>&
PreparedRuntimeEnvironment::variables() const {
    return variables_;
}

const std::filesystem::path& PreparedRuntimeEnvironment::node_file() const {
    return node_file_;
}

} // namespace rlbs
