#pragma once

#include <cstdint>

namespace rlbs
{

struct ResourceRequest
{
    std::uint32_t cpus{1};
    std::uint64_t memory_mb{0};
    std::uint32_t gpus{0};
};

struct ResourceCapacity
{
    std::uint32_t cpus{0};
    std::uint64_t memory_mb{0};
    std::uint32_t gpus{0};
};

[[nodiscard]] bool can_fit(
    const ResourceRequest& request,
    const ResourceCapacity& available
);

}
