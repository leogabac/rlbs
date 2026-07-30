#include <rlbs/cli/native_script.hpp>

#include <rlbs/cli/duration.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <limits>
#include <optional>
#include <ranges>
#include <set>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace rlbs {
namespace {

struct ParserState {
    JobSpec spec;
    std::set<std::string, std::less<>> seen_keys;
    bool saw_version{false};
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

template <typename Integer>
[[nodiscard]] std::expected<Integer, std::string>
parse_integer(std::string_view value, std::string_view field) {
    Integer parsed{};
    const auto result =
        std::from_chars(value.data(), value.data() + value.size(), parsed);

    if (value.empty() || result.ec != std::errc{} ||
        result.ptr != value.data() + value.size()) {
        return std::unexpected{std::string{field} +
                               " needs a non-negative integer"};
    }

    return parsed;
}

[[nodiscard]] std::expected<bool, std::string>
parse_boolean(std::string_view value, std::string_view field) {
    if (value == "true") {
        return true;
    }
    if (value == "false") {
        return false;
    }

    return std::unexpected{std::string{field} + " needs true or false"};
}

[[nodiscard]] std::expected<std::string, std::string>
parse_string(std::string_view value, std::string_view field) {
    if (value.size() < 2 ||
        (value.front() != '"' && value.front() != '\'') ||
        value.back() != value.front()) {
        return std::unexpected{std::string{field} +
                               " needs a quoted string"};
    }

    const char quote = value.front();
    value.remove_prefix(1);
    value.remove_suffix(1);

    if (quote == '\'') {
        // toml literal strings are useful for paths because backslashes stay
        // boring. a quote inside one needs the double-quoted form instead
        if (value.contains('\'')) {
            return std::unexpected{std::string{field} +
                                   " contains an unescaped quote"};
        }

        return std::string{value};
    }

    std::string parsed;
    parsed.reserve(value.size());
    bool escaped = false;

    for (const char character : value) {
        if (!escaped) {
            if (character == '\\') {
                escaped = true;
            } else if (character == '"') {
                return std::unexpected{std::string{field} +
                                       " contains an unescaped quote"};
            } else {
                parsed.push_back(character);
            }
            continue;
        }

        escaped = false;

        switch (character) {
        case '"':
        case '\\':
            parsed.push_back(character);
            break;
        case 'n':
            parsed.push_back('\n');
            break;
        case 'r':
            parsed.push_back('\r');
            break;
        case 't':
            parsed.push_back('\t');
            break;
        default:
            return std::unexpected{std::string{field} +
                                   " contains an unsupported escape"};
        }
    }

    if (escaped) {
        return std::unexpected{std::string{field} +
                               " ends with an unfinished escape"};
    }

    return parsed;
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

[[nodiscard]] std::expected<std::vector<std::string>, std::string>
parse_shebang(std::string_view value) {
    std::vector<std::string> words;
    std::string current;
    std::optional<char> quote;
    bool escaped = false;

    // shebangs normally stay simple, but /usr/bin/env plus quoted arguments
    // are common enough that splitting only on spaces would be needlessly bad
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
        return std::unexpected{"unterminated quote or escape in shebang"};
    }
    if (!current.empty()) {
        words.push_back(std::move(current));
    }
    if (words.empty()) {
        return std::unexpected{"empty script interpreter"};
    }

    return words;
}

[[nodiscard]] std::expected<void, std::string>
remember_key(ParserState& state, std::string_view key) {
    if (!state.seen_keys.emplace(key).second) {
        return std::unexpected{"rlbs key was specified more than once: " +
                               std::string{key}};
    }

    return {};
}

[[nodiscard]] std::expected<void, std::string>
parse_assignment(ParserState& state, std::string_view assignment) {
    const auto equals = assignment.find('=');

    if (equals == std::string_view::npos) {
        return std::unexpected{"rlbs directive needs key = value"};
    }

    const auto key = trim(assignment.substr(0, equals));
    const auto value = trim(assignment.substr(equals + 1));

    if (key.empty() || value.empty()) {
        return std::unexpected{"rlbs directive needs key = value"};
    }
    if (auto remembered = remember_key(state, key); !remembered) {
        return remembered;
    }

    if (key == "version") {
        auto version = parse_integer<std::uint32_t>(value, key);

        if (!version) {
            return std::unexpected{std::move(version.error())};
        }
        if (*version != 1) {
            return std::unexpected{"unsupported rlbs script version: " +
                                   std::to_string(*version)};
        }

        state.saw_version = true;
        return {};
    }
    if (key == "name") {
        auto parsed = parse_string(value, key);

        if (!parsed) {
            return std::unexpected{std::move(parsed.error())};
        }

        state.spec.name = std::move(*parsed);
        return {};
    }
    if (key == "walltime") {
        auto text = parse_string(value, key);

        if (!text) {
            return std::unexpected{std::move(text.error())};
        }

        auto parsed = parse_walltime(*text);

        if (!parsed) {
            return std::unexpected{std::move(parsed.error())};
        }

        state.spec.walltime = *parsed;
        return {};
    }
    if (key == "resources.cpus") {
        auto parsed = parse_integer<std::uint32_t>(value, key);

        if (!parsed) {
            return std::unexpected{std::move(parsed.error())};
        }
        if (*parsed == 0) {
            return std::unexpected{"resources.cpus must be greater than zero"};
        }

        state.spec.resources.cpus = *parsed;
        return {};
    }
    if (key == "resources.memory_mb") {
        auto parsed = parse_integer<std::uint64_t>(value, key);

        if (!parsed) {
            return std::unexpected{std::move(parsed.error())};
        }

        state.spec.resources.memory_mb = *parsed;
        return {};
    }
    if (key == "resources.gpus") {
        auto parsed = parse_integer<std::uint32_t>(value, key);

        if (!parsed) {
            return std::unexpected{std::move(parsed.error())};
        }

        state.spec.resources.gpus = *parsed;
        return {};
    }
    if (key == "working_directory") {
        auto parsed = parse_string(value, key);

        if (!parsed) {
            return std::unexpected{std::move(parsed.error())};
        }

        state.spec.working_directory = *parsed;
        return {};
    }
    if (key == "inherit_environment") {
        auto parsed = parse_boolean(value, key);

        if (!parsed) {
            return std::unexpected{std::move(parsed.error())};
        }

        state.spec.inherit_environment = *parsed;
        return {};
    }
    if (key == "output.stdout" || key == "output.stderr") {
        auto parsed = parse_string(value, key);

        if (!parsed) {
            return std::unexpected{std::move(parsed.error())};
        }

        auto& path = key == "output.stdout" ? state.spec.stdout_path
                                             : state.spec.stderr_path;
        path = std::filesystem::path{std::move(*parsed)};
        return {};
    }
    if (key == "output.append") {
        auto parsed = parse_boolean(value, key);

        if (!parsed) {
            return std::unexpected{std::move(parsed.error())};
        }

        state.spec.append_output = *parsed;
        return {};
    }

    constexpr std::string_view environment_prefix = "environment.";

    if (key.starts_with(environment_prefix)) {
        const auto name = key.substr(environment_prefix.size());

        if (!valid_environment_name(name)) {
            return std::unexpected{"invalid rlbs environment name: " +
                                   std::string{name}};
        }

        auto parsed = parse_string(value, key);

        if (!parsed) {
            return std::unexpected{std::move(parsed.error())};
        }

        state.spec.environment.push_back({
            .name = std::string{name},
            .value = std::move(*parsed),
        });
        return {};
    }

    return std::unexpected{"unknown rlbs script key: " + std::string{key}};
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
        return std::unexpected{"could not resolve rlbs working directory: " +
                               filesystem_error.message()};
    }

    const bool is_directory =
        std::filesystem::is_directory(candidate, filesystem_error);

    if (filesystem_error || !is_directory) {
        return std::unexpected{"rlbs working directory does not exist: " +
                               candidate.string()};
    }

    return candidate.lexically_normal();
}

[[nodiscard]] std::string line_error(std::size_t line, std::string message) {
    return "rlbs script line " + std::to_string(line) + ": " +
           std::move(message);
}

} // namespace

bool is_native_script_path(const std::filesystem::path& script_path) {
    const auto filename = script_path.filename().string();
    return filename.ends_with(".rlbs") || filename.ends_with(".rlbs.sh");
}

std::expected<JobSpec, std::string>
parse_native_script(const std::filesystem::path& script_path,
                    const std::filesystem::path& submission_directory) {
    std::error_code filesystem_error;
    auto absolute_script =
        std::filesystem::absolute(script_path, filesystem_error);

    if (filesystem_error ||
        !std::filesystem::is_regular_file(absolute_script, filesystem_error)) {
        return std::unexpected{"rlbs script does not exist: " +
                               script_path.string()};
    }

    absolute_script = absolute_script.lexically_normal();
    std::ifstream input{absolute_script};

    if (!input) {
        return std::unexpected{"could not read rlbs script: " +
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
            auto shebang = parse_shebang(trim(stripped.substr(2)));

            if (!shebang) {
                return std::unexpected{
                    line_error(line_number, std::move(shebang.error()))};
            }

            interpreter = std::move(*shebang);
            continue;
        }

        if (stripped.starts_with("#RLBS")) {
            if (!directives_open) {
                return std::unexpected{line_error(
                    line_number,
                    "rlbs directives must appear before executable code")};
            }

            if (auto parsed =
                    parse_assignment(state, trim(stripped.substr(5)));
                !parsed) {
                return std::unexpected{
                    line_error(line_number, std::move(parsed.error()))};
            }

            continue;
        }

        // blank lines and ordinary comments are harmless around metadata.
        // actual shell code closes the header so late settings cannot lie
        if (!stripped.empty() && !stripped.starts_with('#')) {
            directives_open = false;
        }
    }

    if (!state.saw_version) {
        return std::unexpected{"rlbs script needs #RLBS version = 1"};
    }

    auto directory =
        resolve_directory(state.spec.working_directory, submission_directory);

    if (!directory) {
        return std::unexpected{std::move(directory.error())};
    }

    state.spec.working_directory = std::move(*directory);
    state.spec.argv = std::move(interpreter);
    state.spec.argv.push_back(absolute_script.string());
    return state.spec;
}

} // namespace rlbs
