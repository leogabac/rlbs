#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <variant>
#include <vector>

#include <rlbs/core/job.hpp>
#include <rlbs/core/node.hpp>
#include <rlbs/core/types.hpp>

namespace rlbs {

inline constexpr std::size_t max_control_frame_size = 1024 * 1024;

enum class ProtocolOperation {
    encode,
    decode,
};

struct ProtocolError {
    ProtocolOperation operation{ProtocolOperation::decode};
    std::string message;
};

struct SubmitRequest {
    JobSpec spec;
};

struct QueueRequest {};

struct StatusRequest {
    JobId job_id{0};
};

struct CancelRequest {
    JobId job_id{0};
};

struct NodesRequest {};

using ControlRequest = std::variant<SubmitRequest, QueueRequest, StatusRequest,
                                    CancelRequest, NodesRequest>;

struct SubmitResponse {
    JobId job_id{0};
};

// queue only needs the fields a human can scan in a table. shipping argv and
// environment for every job would make one innocent queue command pretty huge
struct JobSummary {
    JobId id{0};
    std::string name;
    std::string queue;
    JobState state{JobState::pending};
    ResourceRequest resources;
    std::optional<NodeId> assigned_node;
    std::optional<std::chrono::seconds> walltime;
    std::optional<std::chrono::seconds> execution_time;
};

struct QueueResponse {
    std::vector<JobSummary> jobs;
};

struct StatusResponse {
    Job job;
};

struct CancelResponse {
    JobId job_id{0};
};

struct NodeSummary {
    NodeId id;
    NodeState state{NodeState::offline};
    ResourceCapacity total;
    ResourceCapacity reserved;
    ResourceCapacity allocated;
    ResourceCapacity available;
};

struct NodesResponse {
    std::vector<NodeSummary> nodes;
};

struct ErrorResponse {
    std::string message;
};

using ControlResponse =
    std::variant<SubmitResponse, QueueResponse, StatusResponse, CancelResponse,
                 NodesResponse, ErrorResponse>;

// frames carry their own size even though unix seqpacket already has packet
// boundaries. tcp can reuse the exact bytes later without inventing framing
[[nodiscard]] std::expected<std::vector<std::byte>, ProtocolError>
encode_request(const ControlRequest& request);

[[nodiscard]] std::expected<ControlRequest, ProtocolError>
decode_request(const std::vector<std::byte>& frame);

[[nodiscard]] std::expected<std::vector<std::byte>, ProtocolError>
encode_response(const ControlResponse& response);

[[nodiscard]] std::expected<ControlResponse, ProtocolError>
decode_response(const std::vector<std::byte>& frame);

} // namespace rlbs
