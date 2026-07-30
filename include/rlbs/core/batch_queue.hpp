#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace rlbs {

// enabled controls whether new jobs may enter the queue. started is separate:
// a stopped queue can keep accepting work without actually launching any of it
struct BatchQueue {
    std::string name;
    int priority{0};
    bool enabled{true};
    bool started{true};
    std::optional<std::uint32_t> max_running;
};

} // namespace rlbs
