#include <rlbs/core/job.hpp>
#include <rlbs/core/node.hpp>
#include <rlbs/core/scheduler.hpp>

#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

int failures = 0;

void expect(bool condition, std::string_view message)
{
    if (!condition) {
        std::cerr << "failed: " << message << '\n';
        ++failures;
    }
}

// keep the scheduler tests focused on queue behavior instead of repeating the
// submission fields that are irrelevant to each little resource check
rlbs::Job make_job(
    rlbs::JobId id,
    std::uint64_t queue_sequence,
    std::string name,
    rlbs::ResourceRequest resources = {},
    std::string queue = "default"
)
{
    rlbs::JobSpec spec;
    spec.name = std::move(name);
    spec.resources = resources;
    spec.queue = std::move(queue);

    return {
        .id = id,
        .queue_sequence = queue_sequence,
        .spec = std::move(spec),
        .state = rlbs::JobState::pending,
        .assigned_node = std::nullopt,
        .result = std::nullopt,
        .execution_time = std::nullopt,
    };
}

[[nodiscard]] std::vector<rlbs::BatchQueue> default_queues()
{
    return {{
        .name = "default",
        .priority = 0,
        .enabled = true,
        .started = true,
        .max_running = std::nullopt,
    }};
}

void test_job_state_machine()
{
    auto job = make_job(1, 1, "state-test");

    expect(
        !rlbs::transition(job, rlbs::JobState::running),
        "pending job cannot jump straight to running"
    );
    expect(
        !rlbs::transition(job, rlbs::JobState::failed),
        "pending job cannot fail before an execution attempt"
    );
    expect(
        rlbs::transition(job, rlbs::JobState::assigned),
        "pending job can be assigned"
    );
    expect(
        rlbs::transition(job, rlbs::JobState::starting),
        "assigned job can start"
    );
    expect(
        rlbs::transition(job, rlbs::JobState::running),
        "starting job can run"
    );
    expect(
        rlbs::transition(job, rlbs::JobState::completed),
        "running job can complete"
    );
    expect(
        !rlbs::transition(job, rlbs::JobState::cancelled),
        "terminal job cannot be cancelled again"
    );

    auto pending_job = make_job(2, 2, "cancel-test");
    expect(
        rlbs::transition(pending_job, rlbs::JobState::cancelled),
        "pending job can be cancelled"
    );
}

void test_reserved_resources_and_gpu_slots()
{
    rlbs::Node node{
        "head",
        {
            .cpus = 16,
            .memory_mb = 64'000,
            .gpus = 2,
        },
        {
            .cpus = 4,
            .memory_mb = 8'000,
            .gpus = 1,
        },
    };

    const auto before = node.available();
    expect(before.cpus == 12, "reserved cpus are not schedulable");
    expect(before.memory_mb == 56'000, "reserved memory is not schedulable");
    expect(before.gpus == 1, "reserved gpu is not schedulable");

    const auto allocation = node.allocate({
        .cpus = 8,
        .memory_mb = 16'000,
        .gpus = 1,
    });

    expect(allocation.has_value(), "request fits after reserved resources");
    expect(
        allocation && allocation->gpu_ids == std::vector<std::uint32_t>{1},
        "allocator returns the unreserved gpu id"
    );
    expect(
        !node.can_allocate({.cpus = 1, .memory_mb = 0, .gpus = 1}),
        "allocated gpu cannot be handed out twice"
    );
    expect(allocation && node.release(*allocation), "allocation can be released");
    expect(
        allocation && !node.release(*allocation),
        "same allocation cannot be released twice"
    );

    const auto after = node.available();
    expect(after.cpus == before.cpus, "released cpus return to the node");
    expect(
        after.memory_mb == before.memory_mb,
        "released memory returns to the node"
    );
    expect(after.gpus == before.gpus, "released gpu returns to the node");
}

void test_reserved_resources_cannot_exceed_capacity()
{
    bool threw = false;

    try {
        static_cast<void>(rlbs::Node{
            "broken",
            {.cpus = 4, .memory_mb = 1'000, .gpus = 0},
            {.cpus = 5, .memory_mb = 0, .gpus = 0},
        });
    } catch (const std::invalid_argument&) {
        threw = true;
    }

    expect(threw, "node rejects impossible reserved resources");
}

void test_scheduler_is_fifo_and_deterministic()
{
    std::vector<rlbs::Node> nodes;
    nodes.emplace_back(
        "node-b",
        rlbs::ResourceCapacity{.cpus = 4, .memory_mb = 8'000, .gpus = 0}
    );
    nodes.emplace_back(
        "node-a",
        rlbs::ResourceCapacity{.cpus = 4, .memory_mb = 8'000, .gpus = 0}
    );

    std::vector<rlbs::Job> jobs{
        make_job(
            2,
            2,
            "younger",
            {.cpus = 2, .memory_mb = 1'000, .gpus = 0}
        ),
        make_job(
            1,
            1,
            "older",
            {.cpus = 2, .memory_mb = 1'000, .gpus = 0}
        ),
    };

    const auto assignments = rlbs::FirstFitScheduler{}.schedule(
        jobs, nodes, default_queues(), jobs.size()
    );

    expect(assignments.size() == 2, "both fitting jobs are assigned");
    expect(
        assignments.size() > 0 && assignments[0].job_id == 1,
        "older queue sequence is assigned first"
    );
    expect(
        assignments.size() > 0 && assignments[0].node_id == "node-a",
        "node id defines deterministic first-fit order"
    );
    expect(
        assignments.size() > 1 && assignments[1].node_id == "node-a",
        "first node is filled while it still fits"
    );
    expect(
        jobs[0].state == rlbs::JobState::assigned
            && jobs[1].state == rlbs::JobState::assigned,
        "assigned jobs move out of pending"
    );
}

void test_strict_fifo_blocks_younger_jobs()
{
    std::vector<rlbs::Node> nodes;
    nodes.emplace_back(
        "local",
        rlbs::ResourceCapacity{.cpus = 4, .memory_mb = 8'000, .gpus = 0}
    );

    std::vector<rlbs::Job> jobs{
        make_job(
            1,
            1,
            "too-large",
            {.cpus = 8, .memory_mb = 1'000, .gpus = 0}
        ),
        make_job(
            2,
            2,
            "would-fit",
            {.cpus = 1, .memory_mb = 1'000, .gpus = 0}
        ),
    };

    const auto assignments = rlbs::FirstFitScheduler{}.schedule(
        jobs, nodes, default_queues(), jobs.size()
    );

    expect(assignments.empty(), "blocked head job stops strict fifo queue");
    expect(
        jobs[1].state == rlbs::JobState::pending,
        "younger fitting job stays pending"
    );
}

void test_offline_nodes_are_skipped()
{
    std::vector<rlbs::Node> nodes;
    nodes.emplace_back(
        "node-a",
        rlbs::ResourceCapacity{.cpus = 4, .memory_mb = 8'000, .gpus = 0}
    );
    nodes.emplace_back(
        "node-b",
        rlbs::ResourceCapacity{.cpus = 4, .memory_mb = 8'000, .gpus = 0}
    );
    nodes[0].set_state(rlbs::NodeState::offline);

    std::vector<rlbs::Job> jobs{
        make_job(
            1,
            1,
            "online-only",
            {.cpus = 1, .memory_mb = 1'000, .gpus = 0}
        ),
    };

    const auto assignments = rlbs::FirstFitScheduler{}.schedule(
        jobs, nodes, default_queues(), jobs.size()
    );

    expect(assignments.size() == 1, "job finds an online node");
    expect(
        assignments.size() == 1 && assignments[0].node_id == "node-b",
        "offline first node is skipped"
    );
}

void test_queue_priority_beats_global_arrival_order()
{
    std::vector<rlbs::Node> nodes;
    nodes.emplace_back(
        "local",
        rlbs::ResourceCapacity{.cpus = 2, .memory_mb = 8'000, .gpus = 0}
    );
    std::vector<rlbs::BatchQueue> queues{
        {
            .name = "low",
            .priority = 10,
            .enabled = true,
            .started = true,
            .max_running = std::nullopt,
        },
        {
            .name = "high",
            .priority = 100,
            .enabled = true,
            .started = true,
            .max_running = std::nullopt,
        },
    };
    std::vector<rlbs::Job> jobs{
        make_job(
            1, 1, "older-low",
            {.cpus = 1, .memory_mb = 0, .gpus = 0}, "low"
        ),
        make_job(
            2, 2, "younger-high",
            {.cpus = 1, .memory_mb = 0, .gpus = 0}, "high"
        ),
    };

    const auto assignments =
        rlbs::FirstFitScheduler{}.schedule(jobs, nodes, queues, 1);

    expect(
        assignments.size() == 1 && assignments[0].job_id == 2,
        "higher-priority queue runs before an older lower-priority job"
    );
    expect(
        jobs[0].state == rlbs::JobState::pending,
        "assignment limit leaves the lower-priority job pending"
    );
}

void test_blocked_or_stopped_queue_does_not_freeze_other_queues()
{
    std::vector<rlbs::Node> nodes;
    nodes.emplace_back(
        "local",
        rlbs::ResourceCapacity{.cpus = 2, .memory_mb = 8'000, .gpus = 0}
    );
    std::vector<rlbs::BatchQueue> queues{
        {
            .name = "high",
            .priority = 100,
            .enabled = true,
            .started = true,
            .max_running = std::nullopt,
        },
        {
            .name = "paused",
            .priority = 50,
            .enabled = true,
            .started = false,
            .max_running = std::nullopt,
        },
        {
            .name = "low",
            .priority = 10,
            .enabled = true,
            .started = true,
            .max_running = std::nullopt,
        },
    };
    std::vector<rlbs::Job> jobs{
        make_job(
            1, 1, "blocked-high",
            {.cpus = 4, .memory_mb = 0, .gpus = 0}, "high"
        ),
        make_job(
            2, 2, "younger-high",
            {.cpus = 1, .memory_mb = 0, .gpus = 0}, "high"
        ),
        make_job(
            3, 3, "paused",
            {.cpus = 1, .memory_mb = 0, .gpus = 0}, "paused"
        ),
        make_job(
            4, 4, "low-can-run",
            {.cpus = 1, .memory_mb = 0, .gpus = 0}, "low"
        ),
    };

    const auto assignments =
        rlbs::FirstFitScheduler{}.schedule(jobs, nodes, queues, jobs.size());

    expect(
        assignments.size() == 1 && assignments[0].job_id == 4,
        "blocked and stopped queues do not freeze another queue"
    );
    expect(
        jobs[1].state == rlbs::JobState::pending,
        "blocked queue keeps fifo instead of backfilling its younger job"
    );
    expect(
        jobs[2].state == rlbs::JobState::pending,
        "stopped queue leaves its job pending"
    );
}

void test_queue_running_limit_is_enforced()
{
    std::vector<rlbs::Node> nodes;
    nodes.emplace_back(
        "local",
        rlbs::ResourceCapacity{.cpus = 4, .memory_mb = 8'000, .gpus = 0}
    );
    std::vector<rlbs::BatchQueue> queues{
        {
            .name = "limited",
            .priority = 100,
            .enabled = true,
            .started = true,
            .max_running = 1,
        },
        {
            .name = "other",
            .priority = 10,
            .enabled = true,
            .started = true,
            .max_running = std::nullopt,
        },
    };
    auto active = make_job(
        1, 1, "already-running",
        {.cpus = 1, .memory_mb = 0, .gpus = 0}, "limited"
    );
    active.state = rlbs::JobState::running;
    std::vector<rlbs::Job> jobs{
        std::move(active),
        make_job(
            2, 2, "limited-waiting",
            {.cpus = 1, .memory_mb = 0, .gpus = 0}, "limited"
        ),
        make_job(
            3, 3, "other-runs",
            {.cpus = 1, .memory_mb = 0, .gpus = 0}, "other"
        ),
    };

    const auto assignments =
        rlbs::FirstFitScheduler{}.schedule(jobs, nodes, queues, jobs.size());

    expect(
        assignments.size() == 1 && assignments[0].job_id == 3,
        "full queue running limit lets another queue run"
    );
    expect(
        jobs[1].state == rlbs::JobState::pending,
        "queue limit keeps its next job pending"
    );
}

}

int main()
{
    test_job_state_machine();
    test_reserved_resources_and_gpu_slots();
    test_reserved_resources_cannot_exceed_capacity();
    test_scheduler_is_fifo_and_deterministic();
    test_strict_fifo_blocks_younger_jobs();
    test_offline_nodes_are_skipped();
    test_queue_priority_beats_global_arrival_order();
    test_blocked_or_stopped_queue_does_not_freeze_other_queues();
    test_queue_running_limit_is_enforced();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }

    std::cout << "all core tests passed\n";
    return 0;
}
