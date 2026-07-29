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
}

void test_responses_round_trip() {
    const auto submitted =
        rlbs::encode_response(rlbs::SubmitResponse{.job_id = 42});
    const auto failed =
        rlbs::encode_response(rlbs::ErrorResponse{.message = "nope\nstill no"});

    expect(submitted.has_value(), "submit response encodes");
    expect(failed.has_value(), "error response encodes");

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
    test_responses_round_trip();
    test_bad_frames_are_rejected();

    if (failures == 0) {
        std::cout << "all control protocol tests passed\n";
    }

    return failures == 0 ? 0 : 1;
}
