#include <rlbs/core/job.hpp>
#include <rlbs/core/node.hpp>
#include <rlbs/core/scheduler.hpp>

#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string_view>
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

void test_job_state_machine()
{
    rlbs::Job job{
        .id = 1,
        .queue_sequence = 1,
        .name = "state-test",
        .resources = {},
        .state = rlbs::JobState::pending,
        .assigned_node = std::nullopt,
    };

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

    rlbs::Job pending_job{
        .id = 2,
        .queue_sequence = 2,
        .name = "cancel-test",
        .resources = {},
        .state = rlbs::JobState::pending,
        .assigned_node = std::nullopt,
    };
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
        {
            .id = 2,
            .queue_sequence = 2,
            .name = "younger",
            .resources = {.cpus = 2, .memory_mb = 1'000, .gpus = 0},
            .state = rlbs::JobState::pending,
            .assigned_node = std::nullopt,
        },
        {
            .id = 1,
            .queue_sequence = 1,
            .name = "older",
            .resources = {.cpus = 2, .memory_mb = 1'000, .gpus = 0},
            .state = rlbs::JobState::pending,
            .assigned_node = std::nullopt,
        },
    };

    const auto assignments = rlbs::FirstFitScheduler{}.schedule(jobs, nodes);

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
        {
            .id = 1,
            .queue_sequence = 1,
            .name = "too-large",
            .resources = {.cpus = 8, .memory_mb = 1'000, .gpus = 0},
            .state = rlbs::JobState::pending,
            .assigned_node = std::nullopt,
        },
        {
            .id = 2,
            .queue_sequence = 2,
            .name = "would-fit",
            .resources = {.cpus = 1, .memory_mb = 1'000, .gpus = 0},
            .state = rlbs::JobState::pending,
            .assigned_node = std::nullopt,
        },
    };

    const auto assignments = rlbs::FirstFitScheduler{}.schedule(jobs, nodes);

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
        {
            .id = 1,
            .queue_sequence = 1,
            .name = "online-only",
            .resources = {.cpus = 1, .memory_mb = 1'000, .gpus = 0},
            .state = rlbs::JobState::pending,
            .assigned_node = std::nullopt,
        },
    };

    const auto assignments = rlbs::FirstFitScheduler{}.schedule(jobs, nodes);

    expect(assignments.size() == 1, "job finds an online node");
    expect(
        assignments.size() == 1 && assignments[0].node_id == "node-b",
        "offline first node is skipped"
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

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }

    std::cout << "all core tests passed\n";
    return 0;
}
