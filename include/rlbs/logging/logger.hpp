#pragma once

#include <mutex>
#include <ostream>
#include <string>
#include <string_view>

namespace rlbs {

enum class LogLevel {
    info,
    warning,
    error,
};

// this deliberately stays tiny. journald can collect stderr later, while the
// same lines remain readable when rlbsd is being babysat in a terminal
class Logger {
  public:
    explicit Logger(std::string process_name);
    Logger(std::string process_name, std::ostream& output);

    void write(LogLevel level, std::string_view component,
               std::string_view message);

    void info(std::string_view component, std::string_view message);
    void warning(std::string_view component, std::string_view message);
    void error(std::string_view component, std::string_view message);

  private:
    std::string process_name_;
    std::string hostname_;
    std::ostream* output_{nullptr};
    std::mutex mutex_;
};

} // namespace rlbs
