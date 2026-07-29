#include <rlbs/logging/logger.hpp>

#include <algorithm>
#include <iostream>
#include <sstream>
#include <string_view>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "failed: " << message << '\n';
        ++failures;
    }
}

void test_journal_style_lines() {
    std::ostringstream output;
    rlbs::Logger logger{"rlbsd-test", output};

    logger.info("daemon", "started node=local");
    logger.warning("executor", "job 4 ignored sigterm\nsent sigkill");
    logger.error("database", "write failed");

    const auto lines = output.str();
    expect(lines.contains(" rlbsd-test["),
           "log line includes process name and pid");
    expect(lines.contains("]: INFO daemon: started node=local\n"),
           "info line includes level and component");
    expect(lines.contains(
               "]: WARNING executor: job 4 ignored sigterm\\nsent sigkill\n"),
           "warning line escapes embedded newlines");
    expect(lines.contains("]: ERROR database: write failed\n"),
           "error line includes level and component");
    expect(std::ranges::count(lines, '\n') == 3,
           "each event stays on one log line");
}

} // namespace

int main() {
    test_journal_style_lines();

    if (failures == 0) {
        std::cout << "all logger tests passed\n";
    }

    return failures == 0 ? 0 : 1;
}
