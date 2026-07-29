#include <rlbs/logging/logger.hpp>

#include <array>
#include <chrono>
#include <ctime>
#include <iostream>
#include <sstream>
#include <utility>

#include <unistd.h>

namespace rlbs {
namespace {

[[nodiscard]] std::string hostname() {
    std::array<char, 256> buffer{};

    // gethostname writes into a fixed buffer because this api also apparently
    // predates the idea that strings might want to know their own size
    if (::gethostname(buffer.data(), buffer.size()) != 0) {
        return "localhost";
    }

    buffer.back() = '\0';
    return buffer.data();
}

[[nodiscard]] std::string_view level_name(LogLevel level) {
    switch (level) {
    case LogLevel::info:
        return "INFO";
    case LogLevel::warning:
        return "WARNING";
    case LogLevel::error:
        return "ERROR";
    }

    return "UNKNOWN";
}

[[nodiscard]] std::string timestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm local_time{};

    // localtime_r is the non-global-buffer version. using localtime() here
    // would let two future threads scribble over the same timestamp state
    if (::localtime_r(&time, &local_time) == nullptr) {
        return "??? ?? ??:??:??";
    }

    std::array<char, 32> buffer{};

    if (std::strftime(buffer.data(), buffer.size(), "%b %e %H:%M:%S",
                      &local_time) == 0) {
        return "??? ?? ??:??:??";
    }

    return buffer.data();
}

[[nodiscard]] std::string single_line(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size());

    // log events must stay one event per line even when a job name contains
    // something cursed. keep the character visible instead of silently eating
    // it and making the stored name disagree with the log
    for (const char character : value) {
        if (character == '\n') {
            escaped += "\\n";
        } else if (character == '\r') {
            escaped += "\\r";
        } else {
            escaped += character;
        }
    }

    return escaped;
}

} // namespace

Logger::Logger(std::string process_name)
    : Logger{std::move(process_name), std::clog} {}

Logger::Logger(std::string process_name, std::ostream& output)
    : process_name_{std::move(process_name)}, hostname_{hostname()},
      output_{&output} {}

void Logger::write(LogLevel level, std::string_view component,
                   std::string_view message) {
    // build the entire line before taking the lock. output gets one write, so
    // concurrent job events cannot splice themselves into timestamp soup
    std::ostringstream line;
    line << timestamp() << ' ' << hostname_ << ' ' << process_name_ << '['
         << ::getpid() << "]: " << level_name(level) << ' '
         << single_line(component) << ": " << single_line(message) << '\n';

    const std::scoped_lock lock{mutex_};
    *output_ << line.str();
    output_->flush();
}

void Logger::info(std::string_view component, std::string_view message) {
    write(LogLevel::info, component, message);
}

void Logger::warning(std::string_view component, std::string_view message) {
    write(LogLevel::warning, component, message);
}

void Logger::error(std::string_view component, std::string_view message) {
    write(LogLevel::error, component, message);
}

} // namespace rlbs
