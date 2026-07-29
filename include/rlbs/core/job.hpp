#pragma once

#include <cstdint>
#include <optional>

#include <rlbs/core/job_spec.hpp>
#include <rlbs/core/types.hpp>

namespace rlbs {

enum class JobState {
    pending,
    assigned,
    starting,
    running,
    completed,
    failed,
    cancelled,
};

struct Job {
    JobId id{0};
    std::uint64_t queue_sequence{0};
    JobSpec spec;
    JobState state{JobState::pending};
    std::optional<NodeId> assigned_node;
};

[[nodiscard]] bool is_terminal(JobState state);
[[nodiscard]] bool can_transition(JobState from, JobState to);
[[nodiscard]] bool transition(Job& job, JobState next);

} // namespace rlbs
