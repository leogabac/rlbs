#include <rlbs/cli/submit.hpp>

#include <rlbs/cli/duration.hpp>
#include <rlbs/cli/native_script.hpp>
#include <rlbs/cli/pbs_script.hpp>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <optional>
#include <ranges>
#include <system_error>
#include <utility>
#include <vector>

extern char** environ;

namespace rlbs {
namespace {

template <typename Integer>
[[nodiscard]] std::expected<Integer, std::string>
parse_integer(std::string_view value, std::string_view option) {
    Integer parsed{};
    const auto result =
        std::from_chars(value.data(), value.data() + value.size(), parsed);

    if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) {
        return std::unexpected{std::string{option} +
                               " needs a non-negative integer"};
    }

    return parsed;
}

[[nodiscard]] std::expected<std::string_view, std::string>
take_value(std::span<const std::string_view> arguments, std::size_t& index) {
    if (index + 1 >= arguments.size() || arguments[index + 1] == "--") {
        return std::unexpected{std::string{arguments[index]} +
                               " needs a value"};
    }

    ++index;
    return arguments[index];
}

[[nodiscard]] std::expected<EnvironmentVariable, std::string>
parse_environment(std::string_view value) {
    const auto separator = value.find('=');

    if (separator == std::string_view::npos || separator == 0) {
        return std::unexpected{"--env needs NAME=VALUE"};
    }

    return EnvironmentVariable{
        .name = std::string{value.substr(0, separator)},
        .value = std::string{value.substr(separator + 1)},
    };
}

[[nodiscard]] std::expected<std::filesystem::path, std::string>
resolve_working_directory(
    const std::optional<std::filesystem::path>& requested) {
    std::error_code filesystem_error;
    auto directory =
        requested ? std::filesystem::absolute(*requested, filesystem_error)
                  : std::filesystem::current_path(filesystem_error);

    if (filesystem_error) {
        return std::unexpected{"could not resolve the working directory: " +
                               filesystem_error.message()};
    }

    const bool exists =
        std::filesystem::is_directory(directory, filesystem_error);

    if (filesystem_error || !exists) {
        return std::unexpected{"working directory does not exist: " +
                               directory.string()};
    }

    return directory.lexically_normal();
}

[[nodiscard]] std::string default_job_name(std::string_view executable) {
    auto name = std::filesystem::path{executable}.filename().string();
    return name.empty() ? std::string{"job"} : name;
}

[[nodiscard]] std::vector<EnvironmentVariable> current_environment() {
    std::vector<EnvironmentVariable> environment;

    // -V means the submit client's environment, not whatever rlbsd inherited
    // when somebody started it three hours ago in a different terminal
    for (char** entry = environ; entry != nullptr && *entry != nullptr;
         ++entry) {
        const std::string_view value{*entry};
        const auto separator = value.find('=');

        if (separator != std::string_view::npos && separator != 0) {
            environment.push_back({
                .name = std::string{value.substr(0, separator)},
                .value = std::string{value.substr(separator + 1)},
            });
        }
    }

    return environment;
}

} // namespace

std::expected<SubmitCommand, std::string>
parse_submit_command(std::span<const std::string_view> arguments) {
    SubmitCommand command;
    std::optional<std::filesystem::path> requested_directory;
    bool found_command = false;
    bool found_script = false;
    bool used_native_job_option = false;
    std::optional<std::string> requested_queue;

    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const auto option = arguments[index];

        // everything after -- belongs to the job exactly as written. without
        // this wall, a perfectly normal command flag would become an rlbs flag
        if (option == "--") {
            ++index;

            for (; index < arguments.size(); ++index) {
                command.spec.argv.emplace_back(arguments[index]);
            }

            found_command = true;
            break;
        }

        if (!option.starts_with('-')) {
            if (index + 1 != arguments.size()) {
                return std::unexpected{
                    "job script must be the final submit argument"};
            }
            if (used_native_job_option) {
                return std::unexpected{
                    "command-line job options cannot be mixed with a job "
                    "script yet"};
            }

            auto submission_directory = resolve_working_directory(std::nullopt);

            if (!submission_directory) {
                return std::unexpected{std::move(submission_directory.error())};
            }

            const std::filesystem::path script_path{option};
            auto parsed =
                is_native_script_path(script_path)
                    ? parse_native_script(script_path, *submission_directory)
                    : parse_pbs_script(script_path, *submission_directory,
                                       current_environment());

            if (!parsed) {
                return std::unexpected{std::move(parsed.error())};
            }

            command.spec = std::move(*parsed);
            found_command = true;
            found_script = true;
            break;
        }

        if (option == "--help" || option == "-h") {
            command.show_help = true;
            continue;
        }
        if (option == "--no-inherit-env") {
            used_native_job_option = true;
            command.spec.inherit_environment = false;
            continue;
        }
        if (option == "--inherit-env") {
            used_native_job_option = true;
            command.spec.inherit_environment = true;
            continue;
        }
        if (option == "--append") {
            used_native_job_option = true;
            command.spec.append_output = true;
            continue;
        }

        auto value = take_value(arguments, index);

        if (!value) {
            return std::unexpected{std::move(value.error())};
        }

        if (option == "--socket") {
            command.socket_path = *value;
        } else if (option == "--queue" || option == "-q") {
            if (requested_queue) {
                return std::unexpected{"queue was specified more than once"};
            }

            // keep this outside used_native_job_option: qsub -q and native
            // command-line overrides both need to work in front of a script
            requested_queue = *value;
        } else if (option == "--name") {
            used_native_job_option = true;
            command.spec.name = *value;
        } else if (option == "--cpus") {
            used_native_job_option = true;
            auto parsed = parse_integer<std::uint32_t>(*value, option);
            if (!parsed) {
                return std::unexpected{std::move(parsed.error())};
            }
            command.spec.resources.cpus = *parsed;
        } else if (option == "--memory-mb") {
            used_native_job_option = true;
            auto parsed = parse_integer<std::uint64_t>(*value, option);
            if (!parsed) {
                return std::unexpected{std::move(parsed.error())};
            }
            command.spec.resources.memory_mb = *parsed;
        } else if (option == "--gpus") {
            used_native_job_option = true;
            auto parsed = parse_integer<std::uint32_t>(*value, option);
            if (!parsed) {
                return std::unexpected{std::move(parsed.error())};
            }
            command.spec.resources.gpus = *parsed;
        } else if (option == "--walltime") {
            used_native_job_option = true;
            auto parsed = parse_walltime(*value);

            if (!parsed) {
                return std::unexpected{std::move(parsed.error())};
            }

            command.spec.walltime = *parsed;
        } else if (option == "--cwd") {
            used_native_job_option = true;
            requested_directory = std::filesystem::path{*value};
        } else if (option == "--env") {
            used_native_job_option = true;
            auto variable = parse_environment(*value);

            if (!variable) {
                return std::unexpected{std::move(variable.error())};
            }

            const bool duplicate = std::ranges::any_of(
                command.spec.environment, [&](const auto& existing) {
                    return existing.name == variable->name;
                });

            // sqlite stores one value per name and the runner also treats later
            // values as overrides, so reject duplicates instead of hiding one
            if (duplicate) {
                return std::unexpected{"duplicate environment variable: " +
                                       variable->name};
            }

            command.spec.environment.push_back(std::move(*variable));
        } else if (option == "--stdout") {
            used_native_job_option = true;
            command.spec.stdout_path = std::filesystem::path{*value};
        } else if (option == "--stderr") {
            used_native_job_option = true;
            command.spec.stderr_path = std::filesystem::path{*value};
        } else {
            return std::unexpected{"unknown submit option: " +
                                   std::string{option}};
        }
    }

    if (command.show_help) {
        // help should not demand a fake command just to explain the real syntax
        return command;
    }
    if (!found_command || command.spec.argv.empty()) {
        return std::unexpected{
            "submit needs a job script or -- followed by a command"};
    }
    if (command.socket_path.empty()) {
        return std::unexpected{"--socket cannot be empty"};
    }
    if (command.spec.resources.cpus == 0) {
        return std::unexpected{"--cpus must be greater than zero"};
    }

    if (requested_queue) {
        command.spec.queue = std::move(*requested_queue);
    }
    if (command.spec.queue.empty() ||
        command.spec.queue.find('\0') != std::string::npos) {
        return std::unexpected{"queue name cannot be empty or contain null"};
    }

    if (command.spec.name.empty()) {
        // /bin/python becoming "python" is useful enough for the native default
        // and still leaves --name for jobs that deserve an actual label
        command.spec.name = default_job_name(command.spec.argv.front());
    }

    if (command.spec.name.contains('/') ||
        command.spec.name.find('\0') != std::string::npos) {
        return std::unexpected{"job name cannot contain a slash or null"};
    }

    if (found_script) {
        return command;
    }

    auto directory = resolve_working_directory(requested_directory);

    if (!directory) {
        return std::unexpected{std::move(directory.error())};
    }

    command.spec.working_directory = std::move(*directory);
    return command;
}

std::string_view submit_usage() {
    return R"usage(usage: rlbs submit [options] -- command [arguments...]
       rlbs submit [--socket PATH] JOB.pbs
       rlbs submit [--socket PATH] JOB.rlbs

options:
  --socket PATH          daemon socket (default: /run/rlbs/rlbs.sock)
  --queue NAME, -q NAME  submit to this queue (default: default)
  --name NAME            job name (default: executable filename)
  --cpus N               requested cpus (default: 1)
  --memory-mb N          requested memory in mb (default: 0)
  --gpus N               requested whole gpus (default: 0)
  --walltime HH:MM:SS    maximum execution time
  --cwd PATH             job working directory (default: current directory)
  --env NAME=VALUE       add or override an environment variable
  --no-inherit-env       start from only the supplied --env values
  --inherit-env          inherit the daemon environment (default)
  --stdout PATH          stdout path relative to the job directory
  --stderr PATH          stderr path relative to the job directory
  --append               append instead of truncating output files
  -h, --help             show this help

basic #PBS options:
  -N, -q, -l, -d, -V, -v, -o, and -e

native scripts:
  .rlbs and .rlbs.sh files use #RLBS key = value directives
)usage";
}

std::string_view cli_usage() {
    return R"usage(usage: rlbs COMMAND [options]

commands:
  submit                 submit a batch job
  queue                  list jobs
  status JOB_ID          show one job
  cancel JOB_ID          cancel one job
  nodes                  show node capacity
  queues                 manage batch queues

general:
  --version              show the rlbs version
  -h, --help             show this help
)usage";
}

} // namespace rlbs
