#pragma once

#include <vector>

#include <rlbs/core/job.hpp>
#include <rlbs/core/node.hpp>

namespace rlbs {

struct Assignment {
    JobId job_id{0};
    NodeId node_id;
    ResourceAllocation allocation;
};

class SchedulingPolicy {
  public:
    virtual ~SchedulingPolicy() = default;

    [[nodiscard]] virtual std::vector<Assignment>
    schedule(std::vector<Job>& jobs, std::vector<Node>& nodes) const = 0;
};

class FirstFitScheduler final : public SchedulingPolicy {
  public:
    [[nodiscard]] std::vector<Assignment>
    schedule(std::vector<Job>& jobs, std::vector<Node>& nodes) const override;
};

} // namespace rlbs
