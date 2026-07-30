#include <rlbs/cli/pbs_script.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <limits>
#include <optional>
#include <ranges>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace rlbs {
namespace {

struct ParserState {
    JobSpec spec;
    std::vector<EnvironmentVariable> explicit_environment;
    bool export_all_environment{false};
    bool saw_cpus{false};
    bool saw_memory{false};
    bool saw_gpus{false};
};

[[nodiscard]] std::string_view trim(std::string_view value) {
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.remove_prefix(1);
    }
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.remove_suffix(1);
    }

    return value;
}

[[nodiscard]] std::expected<std::vector<std::string>, std::string>
split_words(std::string_view value) {
    std::vector<std::string> words;
    std::string current;
    std::optional<char> quote;
    bool escaped = false;

    // this is intentionally a small shell-ish tokenizer, not a shell. pbs
    // directives need quoted spaces and backslashes, but absolutely do not
    // need command substitutions waking up inside the submit client
    for (const char character : value) {
        if (escaped) {
            current.push_back(character);
            escaped = false;
            continue;
        }
        if (character == '\\' && quote != '\'') {
            escaped = true;
            continue;
        }
        if (quote) {
            if (character == *quote) {
                quote.reset();
            } else {
                current.push_back(character);
            }
            continue;
        }
        if (character == '\'' || character == '"') {
            quote = character;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(character)) != 0) {
            if (!current.empty()) {
                words.push_back(std::move(current));
                current.clear();
            }
            continue;
        }

        current.push_back(character);
    }

    if (escaped || quote) {
        return std::unexpected{"unterminated quote or escape"};
    }
    if (!current.empty()) {
        words.push_back(std::move(current));
    }

    return words;
}

template <typename Integer>
[[nodiscard]] std::expected<Integer, std::string>
parse_integer(std::string_view value, std::string_view field) {
    Integer parsed{};
    const auto result =
        std::from_chars(value.data(), value.data() + value.size(), parsed);

    if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) {
        return std::unexpected{std::string{field} +
                               " needs a non-negative integer"};
    }

    return parsed;
}

[[nodiscard]] std::expected<std::uint64_t, std::string>
parse_memory_mb(std::string_view value) {
    const auto unit_start =
        std::ranges::find_if(value, [](const char character) {
            return std::isdigit(static_cast<unsigned char>(character)) == 0;
        });
    const auto number_size =
        static_cast<std::size_t>(std::distance(value.begin(), unit_start));
    auto amount = parse_integer<std::uint64_t>(value.substr(0, number_size),
                                               "pbs memory");

    if (!amount) {
        return std::unexpected{std::move(amount.error())};
    }

    std::string unit{value.substr(number_size)};
    std::ranges::transform(unit, unit.begin(), [](const char character) {
        return static_cast<char>(
            std::tolower(static_cast<unsigned char>(character)));
    });

    std::uint64_t multiplier = 1;
    std::uint64_t divisor = 1;

    if (unit.empty() || unit == "b") {
        divisor = 1024 * 1024;
    } else if (unit == "kb" || unit == "k") {
        divisor = 1024;
    } else if (unit == "mb" || unit == "m") {
        multiplier = 1;
    } else if (unit == "gb" || unit == "g") {
        multiplier = 1024;
    } else if (unit == "tb" || unit == "t") {
        multiplier = 1024 * 1024;
    } else {
        return std::unexpected{"unsupported pbs memory unit: " + unit};
    }

    if (*amount > std::numeric_limits<std::uint64_t>::max() / multiplier) {
        return std::unexpected{"pbs memory request is too large"};
    }

    const auto bytes_or_mb = *amount * multiplier;

    // sub-megabyte pbs sizes round upward because rlbs accounts in whole mb.
    // rounding down would quietly promise the job less than it requested
    return divisor == 1
               ? bytes_or_mb
               : bytes_or_mb / divisor + (bytes_or_mb % divisor != 0 ? 1 : 0);
}

[[nodiscard]] std::expected<void, std::string>
set_resource(ParserState& state, std::string_view name,
             std::string_view value) {
    if (name == "ncpus") {
        if (state.saw_cpus) {
            return std::unexpected{"pbs ncpus was specified more than once"};
        }

        auto cpus = parse_integer<std::uint32_t>(value, "pbs ncpus");

        if (!cpus) {
            return std::unexpected{std::move(cpus.error())};
        }
        if (*cpus == 0) {
            return std::unexpected{"pbs ncpus must be greater than zero"};
        }

        state.spec.resources.cpus = *cpus;
        state.saw_cpus = true;
        return {};
    }

    if (name == "mem") {
        if (state.saw_memory) {
            return std::unexpected{"pbs mem was specified more than once"};
        }

        auto memory_mb = parse_memory_mb(value);

        if (!memory_mb) {
            return std::unexpected{std::move(memory_mb.error())};
        }

        state.spec.resources.memory_mb = *memory_mb;
        state.saw_memory = true;
        return {};
    }

    if (name == "ngpus") {
        if (state.saw_gpus) {
            return std::unexpected{"pbs ngpus was specified more than once"};
        }

        auto gpus = parse_integer<std::uint32_t>(value, "pbs ngpus");

        if (!gpus) {
            return std::unexpected{std::move(gpus.error())};
        }

        state.spec.resources.gpus = *gpus;
        state.saw_gpus = true;
        return {};
    }

    return std::unexpected{"unsupported pbs resource: " + std::string{name}};
}

[[nodiscard]] std::expected<void, std::string>
parse_resource_pair(ParserState& state, std::string_view pair) {
    const auto separator = pair.find('=');

    if (separator == std::string_view::npos || separator == 0 ||
        separator + 1 == pair.size()) {
        return std::unexpected{"pbs resource needs NAME=VALUE: " +
                               std::string{pair}};
    }

    return set_resource(state, pair.substr(0, separator),
                        pair.substr(separator + 1));
}

[[nodiscard]] std::expected<void, std::string>
parse_select(ParserState& state, std::string_view value) {
    const auto first_separator = value.find(':');
    const auto count_text = value.substr(0, first_separator);
    auto count = parse_integer<std::uint32_t>(count_text, "pbs select");

    if (!count) {
        return std::unexpected{std::move(count.error())};
    }
    if (*count != 1) {
        return std::unexpected{
            "rlbs local mode supports exactly one pbs select chunk"};
    }
    if (first_separator == std::string_view::npos) {
        return {};
    }

    auto fields = value.substr(first_separator + 1);

    while (!fields.empty()) {
        const auto separator = fields.find(':');
        const auto field = fields.substr(0, separator);

        if (auto parsed = parse_resource_pair(state, field); !parsed) {
            return parsed;
        }

        if (separator == std::string_view::npos) {
            break;
        }

        fields.remove_prefix(separator + 1);
    }

    return {};
}

[[nodiscard]] std::expected<void, std::string>
parse_resources(ParserState& state, std::string_view value) {
    while (!value.empty()) {
        const auto separator = value.find(',');
        const auto resource = value.substr(0, separator);

        if (resource.starts_with("select=")) {
            if (auto parsed = parse_select(state, resource.substr(7));
                !parsed) {
                return parsed;
            }
        } else if (auto parsed = parse_resource_pair(state, resource);
                   !parsed) {
            return parsed;
        }

        if (separator == std::string_view::npos) {
            break;
        }

        value.remove_prefix(separator + 1);
    }

    return {};
}

[[nodiscard]] bool valid_environment_name(std::string_view name) {
    if (name.empty() ||
        (std::isalpha(static_cast<unsigned char>(name.front())) == 0 &&
         name.front() != '_')) {
        return false;
    }

    return std::ranges::all_of(name.substr(1), [](const char character) {
        return std::isalnum(static_cast<unsigned char>(character)) != 0 ||
               character == '_';
    });
}

void set_environment(std::vector<EnvironmentVariable>& environment,
                     EnvironmentVariable variable) {
    const auto existing = std::ranges::find(environment, variable.name,
                                            &EnvironmentVariable::name);

    if (existing != environment.end()) {
        existing->value = std::move(variable.value);
    } else {
        environment.push_back(std::move(variable));
    }
}

[[nodiscard]] std::expected<void, std::string> parse_environment_list(
    ParserState& state, std::string_view value,
    std::span<const EnvironmentVariable> submission_environment) {
    while (!value.empty()) {
        const auto comma = value.find(',');
        const auto entry = value.substr(0, comma);
        const auto equals = entry.find('=');
        const auto name = entry.substr(0, equals);

        if (!valid_environment_name(name)) {
            return std::unexpected{"invalid pbs environment name: " +
                                   std::string{name}};
        }

        std::string variable_value;

        if (equals != std::string_view::npos) {
            variable_value = entry.substr(equals + 1);
        } else {
            const auto inherited = std::ranges::find(
                submission_environment, name, &EnvironmentVariable::name);

            if (inherited == submission_environment.end()) {
                return std::unexpected{"pbs environment variable is not set: " +
                                       std::string{name}};
            }

            variable_value = inherited->value;
        }

        set_environment(state.explicit_environment,
                        {
                            .name = std::string{name},
                            .value = std::move(variable_value),
                        });

        if (comma == std::string_view::npos) {
            break;
        }

        value.remove_prefix(comma + 1);
    }

    return {};
}

[[nodiscard]] std::expected<std::string_view, std::string>
take_option_value(const std::vector<std::string>& words, std::size_t& index,
                  std::string_view option) {
    if (index + 1 >= words.size()) {
        return std::unexpected{std::string{option} + " needs a value"};
    }

    ++index;
    return words[index];
}

[[nodiscard]] std::expected<void, std::string>
parse_directive(ParserState& state, std::string_view directive,
                std::span<const EnvironmentVariable> submission_environment) {
    auto words = split_words(directive);

    if (!words) {
        return std::unexpected{std::move(words.error())};
    }

    for (std::size_t index = 0; index < words->size(); ++index) {
        const auto& option = (*words)[index];

        if (option == "-V") {
            state.export_all_environment = true;
            continue;
        }
        if (option == "-j") {
            return std::unexpected{
                "pbs -j output joining is not supported yet"};
        }

        auto value = take_option_value(*words, index, option);

        if (!value) {
            return std::unexpected{std::move(value.error())};
        }

        if (option == "-N") {
            state.spec.name = *value;
        } else if (option == "-l") {
            if (auto parsed = parse_resources(state, *value); !parsed) {
                return parsed;
            }
        } else if (option == "-d") {
            state.spec.working_directory = *value;
        } else if (option == "-v") {
            if (auto parsed = parse_environment_list(state, *value,
                                                     submission_environment);
                !parsed) {
                return parsed;
            }
        } else if (option == "-o") {
            state.spec.stdout_path = std::filesystem::path{*value};
        } else if (option == "-e") {
            state.spec.stderr_path = std::filesystem::path{*value};
        } else {
            return std::unexpected{"unsupported pbs option: " + option};
        }
    }

    return {};
}

[[nodiscard]] std::expected<std::filesystem::path, std::string>
resolve_directory(const std::filesystem::path& requested,
                  const std::filesystem::path& submission_directory) {
    std::error_code filesystem_error;
    const auto candidate =
        requested.empty() ? submission_directory
        : requested.is_absolute()
            ? requested
            : std::filesystem::absolute(submission_directory / requested,
                                        filesystem_error);

    if (filesystem_error) {
        return std::unexpected{"could not resolve pbs working directory: " +
                               filesystem_error.message()};
    }

    const bool is_directory =
        std::filesystem::is_directory(candidate, filesystem_error);

    if (filesystem_error || !is_directory) {
        return std::unexpected{"pbs working directory does not exist: " +
                               candidate.string()};
    }

    return candidate.lexically_normal();
}

[[nodiscard]] std::string line_error(std::size_t line, std::string message) {
    return "pbs script line " + std::to_string(line) + ": " +
           std::move(message);
}

} // namespace

std::expected<JobSpec, std::string>
parse_pbs_script(const std::filesystem::path& script_path,
                 const std::filesystem::path& submission_directory,
                 std::span<const EnvironmentVariable> submission_environment) {
    std::error_code filesystem_error;
    auto absolute_script =
        std::filesystem::absolute(script_path, filesystem_error);

    if (filesystem_error ||
        !std::filesystem::is_regular_file(absolute_script, filesystem_error)) {
        return std::unexpected{"pbs script does not exist: " +
                               script_path.string()};
    }

    absolute_script = absolute_script.lexically_normal();
    std::ifstream input{absolute_script};

    if (!input) {
        return std::unexpected{"could not read pbs script: " +
                               absolute_script.string()};
    }

    ParserState state;
    state.spec.name = absolute_script.filename().string();
    state.spec.working_directory = submission_directory;
    std::vector<std::string> interpreter{"/bin/sh"};
    bool directives_open = true;
    std::string line;
    std::size_t line_number = 0;

    while (std::getline(input, line)) {
        ++line_number;
        const auto stripped = trim(line);

        if (line_number == 1 && stripped.starts_with("#!")) {
            auto shebang = split_words(stripped.substr(2));

            if (!shebang || shebang->empty()) {
                return std::unexpected{line_error(
                    line_number, shebang ? "empty script interpreter"
                                         : std::move(shebang.error()))};
            }

            interpreter = std::move(*shebang);
            continue;
        }

        if (stripped.starts_with("#PBS")) {
            if (!directives_open) {
                return std::unexpected{line_error(
                    line_number,
                    "pbs directives must appear before executable code")};
            }

            if (auto parsed = parse_directive(state, trim(stripped.substr(4)),
                                              submission_environment);
                !parsed) {
                return std::unexpected{
                    line_error(line_number, std::move(parsed.error()))};
            }

            continue;
        }

        if (!stripped.empty() && !stripped.starts_with('#')) {
            directives_open = false;
        }
    }

    if (!input.eof()) {
        return std::unexpected{"could not finish reading pbs script: " +
                               absolute_script.string()};
    }

    if (state.spec.name.empty() || state.spec.name.contains('/') ||
        state.spec.name.find('\0') != std::string::npos) {
        return std::unexpected{
            "pbs job name cannot be empty or contain a slash or null"};
    }

    auto working_directory =
        resolve_directory(state.spec.working_directory, submission_directory);

    if (!working_directory) {
        return std::unexpected{std::move(working_directory.error())};
    }

    state.spec.working_directory = std::move(*working_directory);
    state.spec.argv = std::move(interpreter);
    state.spec.argv.push_back(absolute_script.string());

    if (state.export_all_environment) {
        state.spec.environment.assign(submission_environment.begin(),
                                      submission_environment.end());
        state.spec.inherit_environment = false;
    }

    for (auto& variable : state.explicit_environment) {
        set_environment(state.spec.environment, std::move(variable));
    }

    return std::move(state.spec);
}

} // namespace rlbs
