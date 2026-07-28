#include <rlbs/core/node.hpp>

#include <stdexcept>
#include <utility>

namespace rlbs {

Node::Node(NodeId id, ResourceCapacity total, ResourceCapacity reserved)
    : id_{std::move(id)}, total_{total}, reserved_{reserved},
      gpu_in_use_(total.gpus, false) {
    if (id_.empty()) {
        throw std::invalid_argument{"node id cannot be empty"};
    }

    if (!is_within(reserved_, total_)) {
        throw std::invalid_argument{"reserved resources exceed node capacity"};
    }

    // reserved gpu slots stay out of the allocator from the beginning. for
    // now these are the lowest ids, because configuration by exact id can
    // wait until we actually have machines annoying enough to need it
    for (std::uint32_t gpu = 0; gpu < reserved_.gpus; ++gpu) {
        gpu_in_use_[gpu] = true;
    }
}

std::string_view Node::id() const { return id_; }

NodeState Node::state() const { return state_; }

void Node::set_state(NodeState state) { state_ = state; }

const ResourceCapacity& Node::total() const { return total_; }

const ResourceCapacity& Node::reserved() const { return reserved_; }

const ResourceCapacity& Node::allocated() const { return allocated_; }

ResourceCapacity Node::available() const {
    // these subtractions are safe because the constructor and allocator keep
    // reserved + allocated below the real capacity instead of hoping callers do
    return {
        .cpus = total_.cpus - reserved_.cpus - allocated_.cpus,
        .memory_mb =
            total_.memory_mb - reserved_.memory_mb - allocated_.memory_mb,
        .gpus = total_.gpus - reserved_.gpus - allocated_.gpus,
    };
}

bool Node::can_allocate(const ResourceRequest& request) const {
    return state_ == NodeState::online && is_valid(request) &&
           can_fit(request, available());
}

std::optional<ResourceAllocation>
Node::allocate(const ResourceRequest& request) {
    if (!can_allocate(request)) {
        return std::nullopt;
    }

    ResourceAllocation allocation{
        .resources = request,
        .gpu_ids = {},
    };
    allocation.gpu_ids.reserve(request.gpus);

    for (std::uint32_t gpu = reserved_.gpus;
         gpu < gpu_in_use_.size() && allocation.gpu_ids.size() < request.gpus;
         ++gpu) {
        if (!gpu_in_use_[gpu]) {
            allocation.gpu_ids.push_back(gpu);
        }
    }

    // the counts and bitmap should agree, but bail out before changing anything
    // if they somehow do not. corrupting accounting here would be much worse
    if (allocation.gpu_ids.size() != request.gpus) {
        return std::nullopt;
    }

    allocated_.cpus += request.cpus;
    allocated_.memory_mb += request.memory_mb;
    allocated_.gpus += request.gpus;

    for (const auto gpu : allocation.gpu_ids) {
        gpu_in_use_[gpu] = true;
    }

    return allocation;
}

bool Node::release(const ResourceAllocation& allocation) {
    if (allocation.gpu_ids.size() != allocation.resources.gpus ||
        allocation.resources.cpus > allocated_.cpus ||
        allocation.resources.memory_mb > allocated_.memory_mb ||
        allocation.resources.gpus > allocated_.gpus) {
        return false;
    }

    std::vector<bool> seen(total_.gpus, false);

    for (const auto gpu : allocation.gpu_ids) {
        if (gpu < reserved_.gpus || gpu >= gpu_in_use_.size() ||
            !gpu_in_use_[gpu] || seen[gpu]) {
            // validate the whole release before touching counters, otherwise a
            // bad duplicate id can leave us in a fun half-released mess
            return false;
        }

        seen[gpu] = true;
    }

    allocated_.cpus -= allocation.resources.cpus;
    allocated_.memory_mb -= allocation.resources.memory_mb;
    allocated_.gpus -= allocation.resources.gpus;

    for (const auto gpu : allocation.gpu_ids) {
        gpu_in_use_[gpu] = false;
    }

    return true;
}

} // namespace rlbs
