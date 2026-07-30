#include <rlbs/local/output_spool.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <fstream>
#include <system_error>
#include <utility>
#include <vector>

#include <unistd.h>

namespace rlbs {
namespace {

[[nodiscard]] OutputSpoolError error(OutputSpoolOperation operation,
                                     std::string message,
                                     int system_error = 0) {
    return {
        .operation = operation,
        .system_error = system_error,
        .message = std::move(message),
    };
}

[[nodiscard]] std::filesystem::path default_output_path(const Job& job,
                                                        char stream) {
    std::string name = job.spec.name.empty() ? "job" : job.spec.name;

    // names are display text, not sneaky little paths. explicit -o/-e still
    // get their real path handling below
    std::replace(name.begin(), name.end(), '/', '_');
    return name + '.' + stream + std::to_string(job.id);
}

[[nodiscard]] std::filesystem::path
resolve_destination(const Job& job,
                    const std::optional<std::filesystem::path>& requested,
                    char stream) {
    const auto path = requested.value_or(default_output_path(job, stream));
    return (path.is_absolute() ? path : job.spec.working_directory / path)
        .lexically_normal();
}

[[nodiscard]] std::expected<void, OutputSpoolError>
copy_file_into(const std::filesystem::path& source, std::ofstream& output) {
    std::ifstream input{source, std::ios::binary};

    if (!input) {
        return std::unexpected{
            error(OutputSpoolOperation::read_output,
                  "could not read output spool " + source.string(), errno)};
    }

    std::array<char, 64 * 1024> buffer{};

    while (input) {
        input.read(buffer.data(),
                   static_cast<std::streamsize>(buffer.size()));
        const auto received = input.gcount();

        if (received > 0) {
            output.write(buffer.data(), received);
        }
    }

    if (input.bad()) {
        return std::unexpected{
            error(OutputSpoolOperation::read_output,
                  "could not finish reading output spool " + source.string(),
                  errno)};
    }
    if (!output) {
        return std::unexpected{
            error(OutputSpoolOperation::write_output,
                  "could not write staged output", errno)};
    }

    return {};
}

class TemporaryStageFile {
  public:
    [[nodiscard]] static std::expected<TemporaryStageFile, OutputSpoolError>
    create(const std::filesystem::path& destination) {
        auto parent = destination.parent_path();

        if (parent.empty()) {
            parent = ".";
        }

        auto pattern =
            (parent / (".rlbs-stage-" + destination.filename().string() +
                       "-XXXXXX"))
                .string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');

        // mkstemp gives us a collision-free name in the destination directory.
        // the data gets assembled there so the final rename stays atomic
        const int descriptor = ::mkstemp(writable.data());

        if (descriptor < 0) {
            return std::unexpected{
                error(OutputSpoolOperation::create_stage_file,
                      "could not create temporary output beside " +
                          destination.string(),
                      errno)};
        }

        static_cast<void>(::close(descriptor));
        return TemporaryStageFile{writable.data()};
    }

    TemporaryStageFile(const TemporaryStageFile&) = delete;
    TemporaryStageFile& operator=(const TemporaryStageFile&) = delete;

    TemporaryStageFile(TemporaryStageFile&& other) noexcept
        : path_{std::exchange(other.path_, {})} {}

    ~TemporaryStageFile() {
        if (!path_.empty()) {
            std::error_code ignored;
            std::filesystem::remove(path_, ignored);
        }
    }

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }
    void release() { path_.clear(); }

  private:
    explicit TemporaryStageFile(std::filesystem::path path)
        : path_{std::move(path)} {}

    std::filesystem::path path_;
};

} // namespace

std::expected<PreparedOutputSpool, OutputSpoolError>
PreparedOutputSpool::create(const std::filesystem::path& spool_root,
                            const Job& job) {
    std::error_code filesystem_error;
    std::filesystem::create_directories(spool_root, filesystem_error);

    if (filesystem_error) {
        return std::unexpected{
            error(OutputSpoolOperation::create_directory,
                  "could not create output spool root " + spool_root.string() +
                      ": " + filesystem_error.message(),
                  filesystem_error.value())};
    }

    auto pattern =
        (spool_root / ("job-" + std::to_string(job.id) + "-XXXXXX")).string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');

    // one private directory per launch keeps two jobs with the same output
    // names from stepping on each other before staging
    const auto* created = ::mkdtemp(writable.data());

    if (created == nullptr) {
        return std::unexpected{
            error(OutputSpoolOperation::create_directory,
                  "could not create output spool for job " +
                      std::to_string(job.id),
                  errno)};
    }

    const std::filesystem::path directory{created};
    const auto stdout_destination =
        resolve_destination(job, job.spec.stdout_path, 'o');
    const auto stderr_destination =
        resolve_destination(job, job.spec.stderr_path, 'e');
    const bool shared_output = stdout_destination == stderr_destination;
    const auto stdout_spool = directory / "stdout";
    const auto stderr_spool =
        shared_output ? stdout_spool : directory / "stderr";

    // create both files now, before launch. even a failure before fork/exec can
    // then stage the same empty outputs instead of falling into a special case
    {
        std::ofstream stdout_file{stdout_spool,
                                  std::ios::binary | std::ios::trunc};

        if (!stdout_file) {
            return std::unexpected{
                error(OutputSpoolOperation::write_output,
                      "could not create stdout spool " +
                          stdout_spool.string(),
                      errno)};
        }
    }
    if (!shared_output) {
        std::ofstream stderr_file{stderr_spool,
                                  std::ios::binary | std::ios::trunc};

        if (!stderr_file) {
            return std::unexpected{
                error(OutputSpoolOperation::write_output,
                      "could not create stderr spool " +
                          stderr_spool.string(),
                      errno)};
        }
    }

    return PreparedOutputSpool{
        directory,
        stdout_spool,
        stderr_spool,
        stdout_destination,
        stderr_destination,
        job.spec.append_output,
        shared_output,
    };
}

PreparedOutputSpool::PreparedOutputSpool(
    std::filesystem::path directory, std::filesystem::path stdout_spool,
    std::filesystem::path stderr_spool,
    std::filesystem::path stdout_destination,
    std::filesystem::path stderr_destination, bool append_output,
    bool shared_output)
    : directory_{std::move(directory)},
      stdout_spool_{std::move(stdout_spool)},
      stderr_spool_{std::move(stderr_spool)},
      stdout_destination_{std::move(stdout_destination)},
      stderr_destination_{std::move(stderr_destination)},
      append_output_{append_output},
      shared_output_{shared_output} {}

const std::filesystem::path& PreparedOutputSpool::stdout_path() const {
    return stdout_spool_;
}

const std::filesystem::path& PreparedOutputSpool::stderr_path() const {
    return stderr_spool_;
}

const std::filesystem::path& PreparedOutputSpool::directory() const {
    return directory_;
}

std::expected<void, OutputSpoolError>
PreparedOutputSpool::stage_one(const std::filesystem::path& source,
                               const std::filesystem::path& destination) {
    auto temporary = TemporaryStageFile::create(destination);

    if (!temporary) {
        return std::unexpected{std::move(temporary.error())};
    }

    std::ofstream output{temporary->path(),
                         std::ios::binary | std::ios::trunc};

    if (!output) {
        return std::unexpected{
            error(OutputSpoolOperation::write_output,
                  "could not open temporary output " +
                      temporary->path().string(),
                  errno)};
    }

    if (append_output_) {
        std::error_code exists_error;
        const bool destination_exists =
            std::filesystem::exists(destination, exists_error);

        if (exists_error) {
            return std::unexpected{
                error(OutputSpoolOperation::read_output,
                      "could not inspect existing output " +
                          destination.string() + ": " + exists_error.message(),
                      exists_error.value())};
        }
        if (destination_exists) {
            if (auto copied = copy_file_into(destination, output); !copied) {
                return copied;
            }
        }
    }

    if (auto copied = copy_file_into(source, output); !copied) {
        return copied;
    }

    output.close();

    if (!output) {
        return std::unexpected{
            error(OutputSpoolOperation::write_output,
                  "could not finish staged output for " +
                      destination.string(),
                  errno)};
    }

    std::error_code rename_error;
    std::filesystem::rename(temporary->path(), destination, rename_error);

    if (rename_error) {
        return std::unexpected{
            error(OutputSpoolOperation::publish_output,
                  "could not publish output " + destination.string() + ": " +
                      rename_error.message(),
                  rename_error.value())};
    }

    temporary->release();
    return {};
}

std::expected<void, OutputSpoolError> PreparedOutputSpool::stage() {
    if (!stdout_staged_) {
        if (auto staged = stage_one(stdout_spool_, stdout_destination_);
            !staged) {
            return staged;
        }

        stdout_staged_ = true;

        if (shared_output_) {
            stderr_staged_ = true;
        }
    }

    if (!stderr_staged_) {
        if (auto staged = stage_one(stderr_spool_, stderr_destination_);
            !staged) {
            return staged;
        }

        stderr_staged_ = true;
    }

    std::error_code remove_error;
    std::filesystem::remove_all(directory_, remove_error);

    if (remove_error) {
        return std::unexpected{
            error(OutputSpoolOperation::clean_spool,
                  "could not remove empty output spool " +
                      directory_.string() + ": " + remove_error.message(),
                  remove_error.value())};
    }

    return {};
}

} // namespace rlbs
