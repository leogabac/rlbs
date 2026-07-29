#pragma once

#include <optional>

namespace rlbs {

// this is how the process actually ended. exit_code is for a normal exit,
// terminating_signal is for a signal, and only one of them should be set
struct JobResult {
    std::optional<int> exit_code;
    std::optional<int> terminating_signal;
    bool dumped_core{false};
};

} // namespace rlbs
