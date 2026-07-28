#include <rlbs/core/resources.hpp>

namespace rlbs
{

bool is_valid(const ResourceRequest& request)
{
    // a zero-cpu job has no useful scheduling meaning, and letting one in
    // makes all the accounting code weird for basically no payoff
    return request.cpus > 0;
}

bool can_fit(
    const ResourceRequest& request,
    const ResourceCapacity& available
)
{
    return request.cpus <= available.cpus
        && request.memory_mb <= available.memory_mb
        && request.gpus <= available.gpus;
}

bool is_within(
    const ResourceCapacity& resources,
    const ResourceCapacity& limit
)
{
    return resources.cpus <= limit.cpus
        && resources.memory_mb <= limit.memory_mb
        && resources.gpus <= limit.gpus;
}

}
