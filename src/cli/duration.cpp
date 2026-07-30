#include <rlbs/cli/duration.hpp>

#include <array>
#include <charconv>
#include <cstdio>
#include <cstdint>
#include <limits>
#include <system_error>

namespace rlbs {
namespace {

[[nodiscard]] std::expected<std::uint64_t, std::string>
parse_piece(std::string_view value) {
    std::uint64_t parsed = 0;
    const auto result =
        std::from_chars(value.data(), value.data() + value.size(), parsed);

    if (value.empty() || result.ec != std::errc{} ||
        result.ptr != value.data() + value.size()) {
        return std::unexpected{"walltime needs HH:MM:SS"};
    }

    return parsed;
}

} // namespace

std::expected<std::chrono::seconds, std::string>
parse_walltime(std::string_view value) {
    const auto first_colon = value.find(':');
    const auto second_colon =
        first_colon == std::string_view::npos
            ? std::string_view::npos
            : value.find(':', first_colon + 1);

    if (first_colon == std::string_view::npos ||
        second_colon == std::string_view::npos ||
        value.find(':', second_colon + 1) != std::string_view::npos) {
        return std::unexpected{"walltime needs HH:MM:SS"};
    }

    auto hours = parse_piece(value.substr(0, first_colon));
    auto minutes =
        parse_piece(value.substr(first_colon + 1, second_colon - first_colon - 1));
    auto seconds = parse_piece(value.substr(second_colon + 1));

    if (!hours || !minutes || !seconds) {
        return std::unexpected{"walltime needs HH:MM:SS"};
    }
    if (*minutes >= 60 || *seconds >= 60) {
        return std::unexpected{
            "walltime minutes and seconds must be less than 60"};
    }

    constexpr auto max_seconds =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());

    if (*hours > max_seconds / 3600 ||
        *hours * 3600 > max_seconds - *minutes * 60 - *seconds) {
        return std::unexpected{"walltime is too large"};
    }

    const auto total = *hours * 3600 + *minutes * 60 + *seconds;

    if (total == 0) {
        return std::unexpected{"walltime must be greater than zero"};
    }

    return std::chrono::seconds{static_cast<std::int64_t>(total)};
}

std::string format_duration(std::chrono::seconds duration) {
    const auto total = static_cast<std::uint64_t>(duration.count());
    const auto hours = total / 3600;
    const auto minutes = total / 60 % 60;
    const auto seconds = total % 60;
    std::array<char, 32> buffer{};
    const auto written =
        std::snprintf(buffer.data(), buffer.size(), "%02llu:%02llu:%02llu",
                      static_cast<unsigned long long>(hours),
                      static_cast<unsigned long long>(minutes),
                      static_cast<unsigned long long>(seconds));

    return written > 0 ? std::string{buffer.data(),
                                     static_cast<std::size_t>(written)}
                       : std::string{"00:00:00"};
}

} // namespace rlbs
