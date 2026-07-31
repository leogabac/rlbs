// queue administration is parsed here and nowhere else. the cli still sends a
// normal daemon request; it never gets a clever shortcut to edit sqlite while
// the scheduler is using it, because that shortcut would become a nightmare.
#include <rlbs/cli/queues.hpp>

#include <charconv>
#include <iomanip>
#include <sstream>
#include <system_error>
#include <utility>

namespace rlbs {
namespace {

[[nodiscard]] std::expected<std::string_view, std::string>
take_value(std::span<const std::string_view> arguments, std::size_t& index) {
    if (index + 1 >= arguments.size()) {
        return std::unexpected{std::string{arguments[index]} +
                               " needs a value"};
    }

    ++index;
    return arguments[index];
}

template <typename Integer>
[[nodiscard]] std::expected<Integer, std::string>
parse_integer(std::string_view value, std::string_view option) {
    Integer parsed{};
    const auto result =
        std::from_chars(value.data(), value.data() + value.size(), parsed);

    if (value.empty() || result.ec != std::errc{} ||
        result.ptr != value.data() + value.size()) {
        return std::unexpected{std::string{option} + " needs an integer"};
    }

    return parsed;
}

[[nodiscard]] std::optional<QueuesCommandAction>
parse_action(std::string_view value) {
    if (value == "add") {
        return QueuesCommandAction::add;
    }
    if (value == "start") {
        return QueuesCommandAction::start;
    }
    if (value == "stop") {
        return QueuesCommandAction::stop;
    }
    if (value == "enable") {
        return QueuesCommandAction::enable;
    }
    if (value == "disable") {
        return QueuesCommandAction::disable;
    }

    return std::nullopt;
}

[[nodiscard]] std::string_view yes_no(bool value) {
    return value ? "yes" : "no";
}

} // namespace

std::expected<QueuesCommand, std::string>
parse_queues_command(std::span<const std::string_view> arguments) {
    QueuesCommand command;
    bool found_action = false;
    bool saw_priority = false;
    bool saw_max_running = false;

    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const auto argument = arguments[index];

        if (argument == "--help" || argument == "-h") {
            command.show_help = true;
            continue;
        }
        if (argument == "--socket") {
            auto value = take_value(arguments, index);

            if (!value) {
                return std::unexpected{std::move(value.error())};
            }

            command.socket_path = *value;
            continue;
        }
        if (argument == "--priority") {
            if (saw_priority) {
                return std::unexpected{"--priority was specified twice"};
            }

            auto value = take_value(arguments, index);

            if (!value) {
                return std::unexpected{std::move(value.error())};
            }

            auto parsed = parse_integer<int>(*value, argument);

            if (!parsed) {
                return std::unexpected{std::move(parsed.error())};
            }

            command.priority = *parsed;
            saw_priority = true;
            continue;
        }
        if (argument == "--max-running") {
            if (saw_max_running) {
                return std::unexpected{"--max-running was specified twice"};
            }

            auto value = take_value(arguments, index);

            if (!value) {
                return std::unexpected{std::move(value.error())};
            }

            auto parsed = parse_integer<std::uint32_t>(*value, argument);

            if (!parsed) {
                return std::unexpected{std::move(parsed.error())};
            }
            if (*parsed == 0) {
                return std::unexpected{
                    "--max-running must be greater than zero"};
            }

            command.max_running = *parsed;
            saw_max_running = true;
            continue;
        }
        if (argument.starts_with('-')) {
            return std::unexpected{"unknown queues option: " +
                                   std::string{argument}};
        }

        if (!found_action) {
            auto action = parse_action(argument);

            if (!action) {
                return std::unexpected{"unknown queues action: " +
                                       std::string{argument}};
            }

            command.action = *action;
            found_action = true;
            continue;
        }
        if (!command.name.empty()) {
            return std::unexpected{"queues accepts one queue name"};
        }

        command.name = argument;
    }

    if (command.show_help) {
        return command;
    }
    if (command.socket_path.empty()) {
        return std::unexpected{"--socket cannot be empty"};
    }
    if (!found_action) {
        if (saw_priority || saw_max_running) {
            return std::unexpected{
                "queue settings need the add action"};
        }

        return command;
    }
    if (command.name.empty() || command.name.find('\0') != std::string::npos) {
        return std::unexpected{
            "queue action needs a non-empty queue name"};
    }
    if (command.action != QueuesCommandAction::add &&
        (saw_priority || saw_max_running)) {
        return std::unexpected{
            "--priority and --max-running only belong to queues add"};
    }

    return command;
}

std::string format_queues(const std::vector<BatchQueue>& queues) {
    std::ostringstream output;
    output << std::left << std::setw(20) << "queue" << std::right
           << std::setw(10) << "priority" << "  " << std::left
           << std::setw(10) << "enabled" << std::setw(10) << "started"
           << "max running\n";

    for (const auto& queue : queues) {
        output << std::left << std::setw(20) << queue.name << std::right
               << std::setw(10) << queue.priority << "  " << std::left
               << std::setw(10) << yes_no(queue.enabled) << std::setw(10)
               << yes_no(queue.started)
               << (queue.max_running ? std::to_string(*queue.max_running)
                                     : "unlimited")
               << '\n';
    }

    return output.str();
}

std::string_view queues_usage() {
    return R"usage(usage: rlbs queues [--socket PATH]
       rlbs queues add NAME [--priority N] [--max-running N] [--socket PATH]
       rlbs queues start NAME [--socket PATH]
       rlbs queues stop NAME [--socket PATH]
       rlbs queues enable NAME [--socket PATH]
       rlbs queues disable NAME [--socket PATH]

enabled controls whether new jobs may enter a queue.
started controls whether waiting jobs may launch.

options:
  --socket PATH          daemon socket (default: /tmp/rlbs.sock)
  --priority N           larger values run first (default: 0)
  --max-running N        maximum active jobs (default: unlimited)
  -h, --help             show this help
)usage";
}

} // namespace rlbs
