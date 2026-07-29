#include <rlbs/cli/submit.hpp>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <optional>
#include <ranges>
#include <system_error>
#include <utility>
#include <vector>

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

} // namespace

std::expected<SubmitCommand, std::string>
parse_submit_command(std::span<const std::string_view> arguments) {
    SubmitCommand command;
    std::optional<std::filesystem::path> requested_directory;
    bool found_command = false;

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

        if (option == "--help" || option == "-h") {
            command.show_help = true;
            continue;
        }
        if (option == "--no-inherit-env") {
            command.spec.inherit_environment = false;
            continue;
        }
        if (option == "--inherit-env") {
            command.spec.inherit_environment = true;
            continue;
        }
        if (option == "--append") {
            command.spec.append_output = true;
            continue;
        }

        auto value = take_value(arguments, index);

        if (!value) {
            return std::unexpected{std::move(value.error())};
        }

        if (option == "--socket") {
            command.socket_path = *value;
        } else if (option == "--name") {
            command.spec.name = *value;
        } else if (option == "--cpus") {
            auto parsed = parse_integer<std::uint32_t>(*value, option);
            if (!parsed) {
                return std::unexpected{std::move(parsed.error())};
            }
            command.spec.resources.cpus = *parsed;
        } else if (option == "--memory-mb") {
            auto parsed = parse_integer<std::uint64_t>(*value, option);
            if (!parsed) {
                return std::unexpected{std::move(parsed.error())};
            }
            command.spec.resources.memory_mb = *parsed;
        } else if (option == "--gpus") {
            auto parsed = parse_integer<std::uint32_t>(*value, option);
            if (!parsed) {
                return std::unexpected{std::move(parsed.error())};
            }
            command.spec.resources.gpus = *parsed;
        } else if (option == "--cwd") {
            requested_directory = std::filesystem::path{*value};
        } else if (option == "--env") {
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
            command.spec.stdout_path = std::filesystem::path{*value};
        } else if (option == "--stderr") {
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
        return std::unexpected{"submit needs -- followed by a command"};
    }
    if (command.socket_path.empty()) {
        return std::unexpected{"--socket cannot be empty"};
    }
    if (command.spec.resources.cpus == 0) {
        return std::unexpected{"--cpus must be greater than zero"};
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

    auto directory = resolve_working_directory(requested_directory);

    if (!directory) {
        return std::unexpected{std::move(directory.error())};
    }

    command.spec.working_directory = std::move(*directory);
    return command;
}

std::string_view submit_usage() {
    return R"usage(usage: rlbs submit [options] -- command [arguments...]

options:
  --socket PATH          daemon socket (default: /tmp/rlbs.sock)
  --name NAME            job name (default: executable filename)
  --cpus N               requested cpus (default: 1)
  --memory-mb N          requested memory in mb (default: 0)
  --gpus N               requested whole gpus (default: 0)
  --cwd PATH             job working directory (default: current directory)
  --env NAME=VALUE       add or override an environment variable
  --no-inherit-env       start from only the supplied --env values
  --inherit-env          inherit the daemon environment (default)
  --stdout PATH          stdout path relative to the job directory
  --stderr PATH          stderr path relative to the job directory
  --append               append instead of truncating output files
  -h, --help             show this help
)usage";
}

std::string_view cli_usage() {
    return R"usage(usage: rlbs COMMAND [options]

commands:
  submit                 submit a batch job
  queue                  list jobs
  status JOB_ID          show one job
  cancel JOB_ID          cancel one job

general:
  --version              show the rlbs version
  -h, --help             show this help
)usage";
}

} // namespace rlbs
