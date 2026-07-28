#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include <rlbs/core/resources.hpp>
#include <rlbs/core/types.hpp>

namespace rlbs {

enum class NodeState {
    online,
    draining,
    offline,
};

struct ResourceAllocation {
    ResourceRequest resources;
    std::vector<std::uint32_t> gpu_ids;
};

class Node {
  public:
    Node(NodeId id, ResourceCapacity total, ResourceCapacity reserved = {});

    [[nodiscard]] std::string_view id() const;
    [[nodiscard]] NodeState state() const;
    void set_state(NodeState state);

    [[nodiscard]] const ResourceCapacity& total() const;
    [[nodiscard]] const ResourceCapacity& reserved() const;
    [[nodiscard]] const ResourceCapacity& allocated() const;
    [[nodiscard]] ResourceCapacity available() const;

    [[nodiscard]] bool can_allocate(const ResourceRequest& request) const;
    [[nodiscard]] std::optional<ResourceAllocation>
    allocate(const ResourceRequest& request);
    [[nodiscard]] bool release(const ResourceAllocation& allocation);

  private:
    NodeId id_;
    ResourceCapacity total_;
    ResourceCapacity reserved_;
    ResourceCapacity allocated_;
    NodeState state_{NodeState::online};
    std::vector<bool> gpu_in_use_;
};

} // namespace rlbs
