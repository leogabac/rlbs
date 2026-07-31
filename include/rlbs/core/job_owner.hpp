// job ownership is server-observed identity, not submission text. keeping it
// separate from JobSpec makes it harder for a client to smuggle in the uid it
// wishes it had and accidentally turn that wish into scheduler truth.
#pragma once

#include <cstdint>

namespace rlbs {

struct JobOwner {
    std::uint32_t user_id{0};
    std::uint32_t group_id{0};

    bool operator==(const JobOwner&) const = default;
};

} // namespace rlbs
