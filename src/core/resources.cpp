#include <rlbs/core/resources.hpp>

namespace rlbs
{

bool can_fit(
    const ResourceRequest& request,
    const ResourceCapacity& available
)
{
    return request.cpus <= available.cpus
        && request.memory_mb <= available.memory_mb
        && request.gpus <= available.gpus;
}

}
