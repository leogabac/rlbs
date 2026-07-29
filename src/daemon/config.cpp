#include <rlbs/daemon/config.hpp>

#include <charconv>
#include <cstdint>
#include <limits>
#include <system_error>
#include <utility>

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
    if (index + 1 >= arguments.size()) {
        return std::unexpected{std::string{arguments[index]} +
                               " needs a value"};
    }

    ++index;
    return arguments[index];
}

} // namespace

std::expected<DaemonConfig, std::string>
parse_daemon_config(std::span<const std::string_view> arguments) {
    DaemonConfig config;

    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const auto option = arguments[index];

        if (option == "--help" || option == "-h") {
            config.show_help = true;
            continue;
        }

        auto value = take_value(arguments, index);

        if (!value) {
            return std::unexpected{std::move(value.error())};
        }

        if (option == "--database") {
            config.database_path = *value;
        } else if (option == "--node-id") {
            config.node_id = *value;
        } else if (option == "--cpus") {
            auto parsed = parse_integer<std::uint32_t>(*value, option);
            if (!parsed) {
                return std::unexpected{std::move(parsed.error())};
            }
            config.capacity.cpus = *parsed;
        } else if (option == "--memory-mb") {
            auto parsed = parse_integer<std::uint64_t>(*value, option);
            if (!parsed) {
                return std::unexpected{std::move(parsed.error())};
            }
            config.capacity.memory_mb = *parsed;
        } else if (option == "--gpus") {
            auto parsed = parse_integer<std::uint32_t>(*value, option);
            if (!parsed) {
                return std::unexpected{std::move(parsed.error())};
            }
            config.capacity.gpus = *parsed;
        } else if (option == "--reserve-cpus") {
            auto parsed = parse_integer<std::uint32_t>(*value, option);
            if (!parsed) {
                return std::unexpected{std::move(parsed.error())};
            }
            config.reserved.cpus = *parsed;
        } else if (option == "--reserve-memory-mb") {
            auto parsed = parse_integer<std::uint64_t>(*value, option);
            if (!parsed) {
                return std::unexpected{std::move(parsed.error())};
            }
            config.reserved.memory_mb = *parsed;
        } else if (option == "--reserve-gpus") {
            auto parsed = parse_integer<std::uint32_t>(*value, option);
            if (!parsed) {
                return std::unexpected{std::move(parsed.error())};
            }
            config.reserved.gpus = *parsed;
        } else if (option == "--tick-ms") {
            auto parsed = parse_integer<std::uint64_t>(*value, option);
            const auto largest_tick = static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max());

            if (!parsed) {
                return std::unexpected{std::move(parsed.error())};
            }
            if (*parsed == 0 || *parsed > largest_tick) {
                return std::unexpected{
                    "--tick-ms needs a positive duration that fits"};
            }

            config.tick_interval =
                std::chrono::milliseconds{static_cast<std::int64_t>(*parsed)};
        } else {
            return std::unexpected{"unknown rlbsd option: " +
                                   std::string{option}};
        }
    }

    if (config.database_path.empty()) {
        return std::unexpected{"--database cannot be empty"};
    }
    if (config.node_id.empty()) {
        return std::unexpected{"--node-id cannot be empty"};
    }
    if (config.capacity.cpus == 0) {
        return std::unexpected{"--cpus must be greater than zero"};
    }
    if (!is_within(config.reserved, config.capacity)) {
        return std::unexpected{
            "reserved resources exceed the local node capacity"};
    }

    return config;
}

std::string_view daemon_usage() {
    return R"usage(usage: rlbsd [options]

options:
  --database PATH           sqlite database path (default: rlbs.db)
  --node-id ID              local node name (default: local)
  --cpus N                  total local cpu capacity (default: 1)
  --memory-mb N             total local memory in mb (default: 0)
  --gpus N                  total local gpu count (default: 0)
  --reserve-cpus N          cpus kept away from batch jobs
  --reserve-memory-mb N     memory kept away from batch jobs
  --reserve-gpus N          gpus kept away from batch jobs
  --tick-ms N               scheduler interval in milliseconds (default: 100)
  -h, --help                show this help
)usage";
}

} // namespace rlbs
