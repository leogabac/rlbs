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

[[nodiscard]] bool is_valid(const ResourceRequest& request);

[[nodiscard]] bool can_fit(
    const ResourceRequest& request,
    const ResourceCapacity& available
);

[[nodiscard]] bool is_within(
    const ResourceCapacity& resources,
    const ResourceCapacity& limit
);

}
