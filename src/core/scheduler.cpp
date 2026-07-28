#include <rlbs/core/scheduler.hpp>

#include <algorithm>
#include <utility>

namespace rlbs
{

std::vector<Assignment> FirstFitScheduler::schedule(
    std::vector<Job>& jobs,
    std::vector<Node>& nodes
) const
{
    std::vector<Job*> pending_jobs;
    pending_jobs.reserve(jobs.size());

    // just append all pending jobs
    for (auto& job : jobs) {
        if (job.state == JobState::pending) {
            pending_jobs.push_back(&job);
        }
    }

    // sort by how they arrived, then by ids if arrived at the same time
    std::ranges::sort(
        pending_jobs,
        {}, // default comparator
        [](const Job* job) {
            return std::pair{job->queue_sequence, job->id};
        }
    );

    std::vector<Node*> ordered_nodes;
    ordered_nodes.reserve(nodes.size());

    for (auto& node : nodes) {
        ordered_nodes.push_back(&node);
    }

    std::ranges::sort(
        ordered_nodes,
        {},
        [](const Node* node) {
            return node->id();
        }
    );

    std::vector<Assignment> assignments;

    for (auto* job : pending_jobs) {
        bool assigned = false;

        for (auto* node : ordered_nodes) {
            //
            // gets an allocation with the resources you need
            auto allocation = node->allocate(job->resources);

            if (!allocation) {
                continue;
            }

            // this transition cannot normally fail because the list only has
            // pending jobs, but undo the allocation if somebody changes that
            // assumption later and forgets this code exists
            if (!transition(*job, JobState::assigned)) {
                static_cast<void>(node->release(*allocation));
                continue;
            }

            job->assigned_node = NodeId{node->id()};
            assignments.push_back({
                .job_id = job->id,
                .node_id = NodeId{node->id()},
                .allocation = std::move(*allocation),
            });
            assigned = true;
            break;
        }

        if (!assigned) {
            // strict fifo means the first blocked job stops the queue. this is
            // deliberately boring now; backfilling can be its own policy later
            break;
        }
    }

    return assignments;
}

}
