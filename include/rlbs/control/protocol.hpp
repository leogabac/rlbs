#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <variant>
#include <vector>

#include <rlbs/core/job_spec.hpp>
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

using ControlRequest = std::variant<SubmitRequest>;

struct SubmitResponse {
    JobId job_id{0};
};

struct ErrorResponse {
    std::string message;
};

using ControlResponse = std::variant<SubmitResponse, ErrorResponse>;

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
