#include <rlbs/core/job.hpp>

namespace rlbs {

bool is_terminal(JobState state) {
    // we want to terminate whenever any of these states happen
    return state == JobState::completed || state == JobState::failed ||
           state == JobState::cancelled;
}

bool can_transition(JobState from, JobState to) {
    // this is just a sanity-check to verify if we can actually transition from, and to
    // the requested states
    if (is_terminal(from)) {
        return false;
    }

    if (to == JobState::cancelled) {
        // cancellation can beat the runner to pretty much any live state, so
        // making it wait for "running" would just create pointless races
        return true;
    }

    if (to == JobState::failed) {
        return from == JobState::assigned || from == JobState::starting ||
               from == JobState::running;
    }

    switch (from) {
    case JobState::pending:
        return to == JobState::assigned;
    case JobState::assigned:
        return to == JobState::starting;
    case JobState::starting:
        return to == JobState::running;
    case JobState::running:
        return to == JobState::completed;
    case JobState::completed:
    case JobState::failed:
    case JobState::cancelled:
        return false;
    }

    return false;
}

bool transition(Job& job, JobState next) {
    if (!can_transition(job.state, next)) {
        return false;
    }

    job.state = next;
    return true;
}

} // namespace rlbs
