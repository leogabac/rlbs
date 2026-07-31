#include <rlbs/control/protocol.hpp>

#include <cstddef>
#include <iostream>
#include <string_view>
#include <variant>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "failed: " << message << '\n';
        ++failures;
    }
}

[[nodiscard]] rlbs::JobSpec example_spec() {
    return {
        .name = "protocol job",
        .resources =
            {
                .cpus = 3,
                .memory_mb = 2048,
                .gpus = 1,
            },
        .argv = {"/bin/sh", "-c", "printf 'line one\\nline two\\n'"},
        .working_directory = "/tmp/protocol work",
        .environment =
            {
                {.name = "FIRST", .value = "one"},
                {.name = "MULTILINE", .value = "two\nthree"},
            },
        .inherit_environment = false,
        .stdout_path = "job output.txt",
        .stderr_path = std::nullopt,
        .append_output = true,
        .walltime = std::chrono::seconds{90},
        .queue = "short",
    };
}

void test_submit_request_round_trip() {
    const auto expected = example_spec();
    const auto frame = rlbs::encode_request(
        rlbs::ControlRequest{rlbs::SubmitRequest{.spec = expected}});

    expect(frame.has_value(), "submit request encodes");

    if (!frame) {
        return;
    }

    const auto decoded = rlbs::decode_request(*frame);
    expect(decoded.has_value(), "submit request decodes");

    if (!decoded) {
        return;
    }

    const auto& actual = std::get<rlbs::SubmitRequest>(*decoded).spec;
    expect(actual.name == expected.name, "request keeps the job name");
    expect(actual.resources.cpus == expected.resources.cpus,
           "request keeps cpus");
    expect(actual.resources.memory_mb == expected.resources.memory_mb,
           "request keeps memory");
    expect(actual.resources.gpus == expected.resources.gpus,
           "request keeps gpus");
    expect(actual.argv == expected.argv, "request keeps argv");
    expect(actual.working_directory == expected.working_directory,
           "request keeps the working directory");
    expect(actual.environment.size() == expected.environment.size(),
           "request keeps the environment");

    if (actual.environment.size() == expected.environment.size()) {
        expect(actual.environment[1].value == expected.environment[1].value,
               "request keeps newlines in environment values");
    }
    expect(actual.inherit_environment == expected.inherit_environment,
           "request keeps environment inheritance");
    expect(actual.stdout_path == expected.stdout_path, "request keeps stdout");
    expect(actual.stderr_path == expected.stderr_path,
           "request keeps absent stderr");
    expect(actual.append_output == expected.append_output,
           "request keeps append mode");
    expect(actual.walltime == expected.walltime,
           "request keeps walltime");
    expect(actual.queue == expected.queue, "request keeps the queue");
}

void test_query_requests_round_trip() {
    const auto queue =
        rlbs::encode_request(rlbs::ControlRequest{rlbs::QueueRequest{}});
    const auto status = rlbs::encode_request(
        rlbs::ControlRequest{rlbs::StatusRequest{.job_id = 73}});
    const auto cancel = rlbs::encode_request(
        rlbs::ControlRequest{rlbs::CancelRequest{.job_id = 74}});
    const auto nodes =
        rlbs::encode_request(rlbs::ControlRequest{rlbs::NodesRequest{}});

    expect(queue.has_value(), "queue request encodes");
    expect(status.has_value(), "status request encodes");
    expect(cancel.has_value(), "cancel request encodes");
    expect(nodes.has_value(), "nodes request encodes");

    if (queue) {
        const auto decoded = rlbs::decode_request(*queue);
        expect(decoded && std::holds_alternative<rlbs::QueueRequest>(*decoded),
               "queue request keeps its type");
    }

    if (status) {
        const auto decoded = rlbs::decode_request(*status);
        expect(decoded && std::get<rlbs::StatusRequest>(*decoded).job_id == 73,
               "status request keeps its job id");
    }

    if (cancel) {
        const auto decoded = rlbs::decode_request(*cancel);
        expect(decoded && std::get<rlbs::CancelRequest>(*decoded).job_id == 74,
               "cancel request keeps its job id");
    }

    if (nodes) {
        const auto decoded = rlbs::decode_request(*nodes);
        expect(decoded && std::holds_alternative<rlbs::NodesRequest>(*decoded),
               "nodes request keeps its type");
    }
}

void test_queue_admin_requests_round_trip() {
    const auto list =
        rlbs::encode_request(rlbs::ControlRequest{rlbs::QueuesRequest{}});
    const auto add = rlbs::encode_request(rlbs::ControlRequest{
        rlbs::AddQueueRequest{
            .queue =
                {
                    .name = "short",
                    .priority = -10,
                    .enabled = true,
                    .started = false,
                    .max_running = 4,
                },
        },
    });
    const auto update = rlbs::encode_request(rlbs::ControlRequest{
        rlbs::UpdateQueueRequest{
            .name = "short",
            .action = rlbs::QueueAction::disable,
        },
    });

    expect(list.has_value(), "queue admin list request encodes");
    expect(add.has_value(), "queue admin add request encodes");
    expect(update.has_value(), "queue admin update request encodes");

    if (list) {
        const auto decoded = rlbs::decode_request(*list);
        expect(decoded &&
                   std::holds_alternative<rlbs::QueuesRequest>(*decoded),
               "queue admin list keeps its type");
    }
    if (add) {
        const auto decoded = rlbs::decode_request(*add);
        expect(decoded.has_value(), "queue admin add decodes");

        if (decoded) {
            const auto& queue =
                std::get<rlbs::AddQueueRequest>(*decoded).queue;
            expect(queue.name == "short", "queue admin add keeps its name");
            expect(queue.priority == -10,
                   "queue admin add keeps signed priority");
            expect(!queue.started, "queue admin add keeps started state");
            expect(queue.max_running == 4,
                   "queue admin add keeps its running limit");
        }
    }
    if (update) {
        const auto decoded = rlbs::decode_request(*update);
        expect(decoded &&
                   std::get<rlbs::UpdateQueueRequest>(*decoded).action ==
                       rlbs::QueueAction::disable,
               "queue admin update keeps its action");
    }
}

void test_responses_round_trip() {
    const auto submitted =
        rlbs::encode_response(rlbs::SubmitResponse{.job_id = 42});
    const auto failed =
        rlbs::encode_response(rlbs::ErrorResponse{.message = "nope\nstill no"});
    const auto cancelled =
        rlbs::encode_response(rlbs::CancelResponse{.job_id = 43});

    expect(submitted.has_value(), "submit response encodes");
    expect(failed.has_value(), "error response encodes");
    expect(cancelled.has_value(), "cancel response encodes");

    if (submitted) {
        const auto decoded = rlbs::decode_response(*submitted);
        expect(decoded && std::get<rlbs::SubmitResponse>(*decoded).job_id == 42,
               "submit response keeps the job id");
    }

    if (failed) {
        const auto decoded = rlbs::decode_response(*failed);
        expect(decoded && std::get<rlbs::ErrorResponse>(*decoded).message ==
                              "nope\nstill no",
               "error response keeps its message");
    }

    if (cancelled) {
        const auto decoded = rlbs::decode_response(*cancelled);
        expect(decoded && std::get<rlbs::CancelResponse>(*decoded).job_id == 43,
               "cancel response keeps its job id");
    }
}

void test_query_responses_round_trip() {
    const rlbs::QueueResponse expected_queue{
        .jobs =
            {
                {
                    .id = 11,
                    .name = "queued",
                    .queue = "short",
                    .state = rlbs::JobState::pending,
                    .resources = {.cpus = 2, .memory_mb = 1024, .gpus = 0},
                    .assigned_node = std::nullopt,
                    .walltime = std::chrono::seconds{300},
                    .execution_time = std::nullopt,
                    .owner =
                        rlbs::JobOwner{
                            .user_id = 1000,
                            .group_id = 100,
                        },
                },
                {
                    .id = 12,
                    .name = "active",
                    .queue = "long",
                    .state = rlbs::JobState::running,
                    .resources = {.cpus = 4, .memory_mb = 8192, .gpus = 1},
                    .assigned_node = "head",
                    .walltime = std::chrono::seconds{600},
                    .execution_time = std::chrono::seconds{17},
                    .owner = std::nullopt,
                },
            },
    };
    const rlbs::Job expected_job{
        .id = 12,
        .queue_sequence = 8,
        .spec = example_spec(),
        .state = rlbs::JobState::completed,
        .assigned_node = "head",
        .result =
            rlbs::JobResult{
                .exit_code = 7,
                .terminating_signal = std::nullopt,
                .dumped_core = false,
            },
        .execution_time = std::chrono::seconds{17},
        .owner =
            rlbs::JobOwner{
                .user_id = 1000,
                .group_id = 100,
            },
    };
    const auto queue = rlbs::encode_response(expected_queue);
    const auto status =
        rlbs::encode_response(rlbs::StatusResponse{.job = expected_job});
    const auto nodes = rlbs::encode_response(rlbs::NodesResponse{
        .nodes =
            {
                {
                    .id = "head",
                    .state = rlbs::NodeState::online,
                    .total = {.cpus = 8, .memory_mb = 32768, .gpus = 2},
                    .reserved = {.cpus = 2, .memory_mb = 4096, .gpus = 1},
                    .allocated = {.cpus = 4, .memory_mb = 8192, .gpus = 0},
                    .available = {.cpus = 2, .memory_mb = 20480, .gpus = 1},
                },
            },
    });

    expect(queue.has_value(), "queue response encodes");
    expect(status.has_value(), "status response encodes");
    expect(nodes.has_value(), "nodes response encodes");

    if (queue) {
        const auto decoded = rlbs::decode_response(*queue);
        expect(decoded && std::get<rlbs::QueueResponse>(*decoded).jobs.size() ==
                              expected_queue.jobs.size(),
               "queue response keeps every job");

        if (decoded) {
            const auto& jobs = std::get<rlbs::QueueResponse>(*decoded).jobs;

            if (jobs.size() == expected_queue.jobs.size()) {
                expect(jobs[0].name == "queued",
                       "queue response keeps job names");
                expect(jobs[0].queue == "short",
                       "queue response keeps queue names");
                expect(jobs[1].assigned_node == "head",
                       "queue response keeps assigned nodes");
                expect(jobs[1].execution_time == std::chrono::seconds{17},
                       "queue response keeps execution time");
                expect(jobs[0].owner &&
                           jobs[0].owner->user_id == 1000,
                       "queue response keeps job ownership");
            }
        }
    }

    if (status) {
        const auto decoded = rlbs::decode_response(*status);
        expect(decoded.has_value(), "status response decodes");

        if (decoded) {
            const auto& job = std::get<rlbs::StatusResponse>(*decoded).job;
            expect(job.id == expected_job.id, "status response keeps job id");
            expect(job.spec.argv == expected_job.spec.argv,
                   "status response keeps argv");
            expect(job.state == rlbs::JobState::completed,
                   "status response keeps state");
            expect(job.result && job.result->exit_code == 7,
                   "status response keeps process results");
            expect(job.execution_time == expected_job.execution_time,
                   "status response keeps execution time");
            expect(job.owner == expected_job.owner,
                   "status response keeps job ownership");
        }
    }

    if (nodes) {
        const auto decoded = rlbs::decode_response(*nodes);
        expect(decoded.has_value(), "nodes response decodes");

        if (decoded) {
            const auto& node =
                std::get<rlbs::NodesResponse>(*decoded).nodes.front();
            expect(node.id == "head", "nodes response keeps the node id");
            expect(node.state == rlbs::NodeState::online,
                   "nodes response keeps node state");
            expect(node.total.cpus == 8, "nodes response keeps total capacity");
            expect(node.reserved.memory_mb == 4096,
                   "nodes response keeps reserved capacity");
            expect(node.allocated.cpus == 4,
                   "nodes response keeps allocated capacity");
            expect(node.available.gpus == 1,
                   "nodes response keeps available capacity");
        }
    }
}

void test_queue_admin_responses_round_trip() {
    const auto listed = rlbs::encode_response(rlbs::QueuesResponse{
        .queues =
            {
                {
                    .name = "short",
                    .priority = 100,
                    .enabled = true,
                    .started = true,
                    .max_running = 4,
                },
            },
    });
    const auto updated = rlbs::encode_response(rlbs::QueueUpdatedResponse{
        .queue =
            {
                .name = "short",
                .priority = 100,
                .enabled = false,
                .started = true,
                .max_running = 4,
            },
    });

    expect(listed.has_value(), "queue admin list response encodes");
    expect(updated.has_value(), "queue admin update response encodes");

    if (listed) {
        const auto decoded = rlbs::decode_response(*listed);
        expect(decoded &&
                   std::get<rlbs::QueuesResponse>(*decoded).queues.size() == 1,
               "queue admin list response keeps every queue");
    }
    if (updated) {
        const auto decoded = rlbs::decode_response(*updated);
        expect(decoded.has_value(), "queue admin update response decodes");

        if (decoded) {
            const auto& queue =
                std::get<rlbs::QueueUpdatedResponse>(*decoded).queue;
            expect(!queue.enabled,
                   "queue admin update response keeps enabled state");
        }
    }
}

void test_bad_frames_are_rejected() {
    auto frame = rlbs::encode_request(
        rlbs::ControlRequest{rlbs::SubmitRequest{.spec = example_spec()}});

    if (!frame) {
        expect(false, "bad frame fixture encodes");
        return;
    }

    frame->pop_back();
    expect(!rlbs::decode_request(*frame), "truncated request is rejected");

    std::vector<std::byte> oversized(rlbs::max_control_frame_size + 1);
    expect(!rlbs::decode_request(oversized), "oversized request is rejected");
}

} // namespace

int main() {
    test_submit_request_round_trip();
    test_query_requests_round_trip();
    test_queue_admin_requests_round_trip();
    test_responses_round_trip();
    test_query_responses_round_trip();
    test_queue_admin_responses_round_trip();
    test_bad_frames_are_rejected();

    if (failures == 0) {
        std::cout << "all control protocol tests passed\n";
    }

    return failures == 0 ? 0 : 1;
}
