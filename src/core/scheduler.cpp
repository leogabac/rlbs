#include <rlbs/core/scheduler.hpp>

#include <algorithm>
#include <map>
#include <set>
#include <string_view>
#include <utility>

namespace rlbs
{

std::vector<Assignment> FirstFitScheduler::schedule(
    std::vector<Job>& jobs,
    std::vector<Node>& nodes,
    const std::vector<BatchQueue>& queues,
    std::size_t max_assignments
) const
{
    if (max_assignments == 0) {
        return {};
    }

    std::map<std::string_view, const BatchQueue*, std::less<>> queue_by_name;
    std::map<std::string_view, std::size_t, std::less<>> active_by_queue;

    for (const auto& queue : queues) {
        queue_by_name.emplace(queue.name, &queue);
    }

    // assigned and starting jobs already own their place even if the process
    // has not quite reached running yet. not counting them would let a busy
    // tick briefly stroll straight past max_running
    for (const auto& job : jobs) {
        if (job.state == JobState::assigned ||
            job.state == JobState::starting ||
            job.state == JobState::running) {
            ++active_by_queue[job.spec.queue];
        }
    }

    std::vector<Job*> pending_jobs;
    pending_jobs.reserve(jobs.size());

    // unknown queues should have been rejected by persistence. skip one here
    // anyway instead of dereferencing wishful thinking if the db was edited
    for (auto& job : jobs) {
        const auto queue = queue_by_name.find(job.spec.queue);

        if (job.state == JobState::pending && queue != queue_by_name.end() &&
            queue->second->started) {
            pending_jobs.push_back(&job);
        }
    }

    // priority chooses between queues. sequence still chooses within one queue
    // and also keeps equal-priority queues globally fifo instead of secretly
    // making alphabetic queue names into another priority setting
    std::ranges::sort(
        pending_jobs,
        [&](const Job* left, const Job* right) {
            const auto* left_queue = queue_by_name.at(left->spec.queue);
            const auto* right_queue = queue_by_name.at(right->spec.queue);

            if (left_queue->priority != right_queue->priority) {
                return left_queue->priority > right_queue->priority;
            }

            return std::pair{left->queue_sequence, left->id} <
                   std::pair{right->queue_sequence, right->id};
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
    std::set<std::string_view, std::less<>> blocked_queues;

    for (auto* job : pending_jobs) {
        const auto* queue = queue_by_name.at(job->spec.queue);
        auto& active = active_by_queue[job->spec.queue];

        if (blocked_queues.contains(job->spec.queue)) {
            continue;
        }
        if (queue->max_running && active >= *queue->max_running) {
            blocked_queues.insert(job->spec.queue);
            continue;
        }

        bool assigned = false;

        for (auto* node : ordered_nodes) {
            //
            // gets an allocation with the resources you need
            auto allocation = node->allocate(job->spec.resources);

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
            ++active;
            assigned = true;
            break;
        }

        if (!assigned) {
            // strict fifo is per queue now. a blocked short-queue head stops
            // younger short jobs, but it should not freeze every other queue
            blocked_queues.insert(job->spec.queue);
        }

        if (assignments.size() >= max_assignments) {
            break;
        }
    }

    return assignments;
}

}
