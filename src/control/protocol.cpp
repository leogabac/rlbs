#include <rlbs/control/protocol.hpp>

// control packets are deliberately explicit and mildly tedious. silently
// guessing an admin request after a version mismatch would be much worse than
// making both sides restart and agree on the exact bytes.
#include <limits>
#include <bit>
#include <optional>
#include <string_view>
#include <utility>

namespace rlbs {
namespace {

constexpr std::uint32_t protocol_magic = 0x524c4253;
constexpr std::uint16_t protocol_version = 5;
constexpr std::uint8_t submit_request_type = 1;
constexpr std::uint8_t queue_request_type = 2;
constexpr std::uint8_t status_request_type = 3;
constexpr std::uint8_t cancel_request_type = 4;
constexpr std::uint8_t nodes_request_type = 5;
constexpr std::uint8_t queues_request_type = 6;
constexpr std::uint8_t add_queue_request_type = 7;
constexpr std::uint8_t update_queue_request_type = 8;
constexpr std::uint8_t submit_response_type = 129;
constexpr std::uint8_t queue_response_type = 130;
constexpr std::uint8_t status_response_type = 131;
constexpr std::uint8_t cancel_response_type = 132;
constexpr std::uint8_t nodes_response_type = 133;
constexpr std::uint8_t queues_response_type = 134;
constexpr std::uint8_t queue_updated_response_type = 135;
constexpr std::uint8_t error_response_type = 255;
constexpr std::uint32_t max_collection_size = 65536;

[[nodiscard]] ProtocolError error(ProtocolOperation operation,
                                  std::string message) {
    return {
        .operation = operation,
        .message = std::move(message),
    };
}

class Writer {
  public:
    // multi-byte integers go out most-significant byte first. host byte order
    // varies by cpu, so copying raw integers would make the protocol depend on
    // whichever machine compiled rlbs, because apparently we need that trap too
    void integer8(std::uint8_t value) {
        bytes_.push_back(static_cast<std::byte>(value));
    }

    void integer16(std::uint16_t value) {
        integer8(static_cast<std::uint8_t>((value >> 8) & 0xff));
        integer8(static_cast<std::uint8_t>(value & 0xff));
    }

    void integer32(std::uint32_t value) {
        integer8(static_cast<std::uint8_t>((value >> 24) & 0xff));
        integer8(static_cast<std::uint8_t>((value >> 16) & 0xff));
        integer8(static_cast<std::uint8_t>((value >> 8) & 0xff));
        integer8(static_cast<std::uint8_t>(value & 0xff));
    }

    void integer64(std::uint64_t value) {
        integer32(static_cast<std::uint32_t>(value >> 32));
        integer32(static_cast<std::uint32_t>(value & 0xffffffff));
    }

    [[nodiscard]] std::expected<void, ProtocolError>
    text(std::string_view value) {
        if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
            return std::unexpected{
                error(ProtocolOperation::encode, "string is too large")};
        }

        // strings are length plus raw bytes, not null-terminated c strings.
        // spaces, newlines, and embedded nulls therefore need no escaping
        integer32(static_cast<std::uint32_t>(value.size()));

        if (!value.empty()) {
            const auto* begin =
                reinterpret_cast<const std::byte*>(value.data());
            bytes_.insert(bytes_.end(), begin, begin + value.size());
        }

        return {};
    }

    [[nodiscard]] std::expected<void, ProtocolError>
    optional_text(const std::optional<std::filesystem::path>& value) {
        integer8(value ? 1 : 0);

        if (!value) {
            return {};
        }

        return text(value->string());
    }

    [[nodiscard]] std::expected<void, ProtocolError>
    optional_text(const std::optional<std::string>& value) {
        integer8(value ? 1 : 0);

        if (!value) {
            return {};
        }

        return text(*value);
    }

    void append(const std::vector<std::byte>& bytes) {
        bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
    }

    [[nodiscard]] const std::vector<std::byte>& bytes() const { return bytes_; }
    [[nodiscard]] std::vector<std::byte> take() { return std::move(bytes_); }

  private:
    std::vector<std::byte> bytes_;
};

class Reader {
  public:
    explicit Reader(const std::vector<std::byte>& bytes) : bytes_{bytes} {}

    [[nodiscard]] std::expected<std::uint8_t, ProtocolError> integer8() {
        // every read checks what remains before advancing position_. missing
        // one check here turns a malformed packet into an out-of-bounds read
        if (remaining() < 1) {
            return truncated();
        }

        return std::to_integer<std::uint8_t>(bytes_[position_++]);
    }

    [[nodiscard]] std::expected<std::uint16_t, ProtocolError> integer16() {
        auto high = integer8();
        auto low = integer8();

        if (!high) {
            return std::unexpected{std::move(high.error())};
        }
        if (!low) {
            return std::unexpected{std::move(low.error())};
        }

        return static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(*high) << 8) | *low);
    }

    [[nodiscard]] std::expected<std::uint32_t, ProtocolError> integer32() {
        if (remaining() < 4) {
            return truncated();
        }

        std::uint32_t value = 0;

        for (int byte = 0; byte < 4; ++byte) {
            value = (value << 8) |
                    std::to_integer<std::uint8_t>(bytes_[position_++]);
        }

        return value;
    }

    [[nodiscard]] std::expected<std::uint64_t, ProtocolError> integer64() {
        auto high = integer32();
        auto low = integer32();

        if (!high) {
            return std::unexpected{std::move(high.error())};
        }
        if (!low) {
            return std::unexpected{std::move(low.error())};
        }

        return (static_cast<std::uint64_t>(*high) << 32) | *low;
    }

    [[nodiscard]] std::expected<std::string, ProtocolError> text() {
        auto size = integer32();

        if (!size) {
            return std::unexpected{std::move(size.error())};
        }
        if (*size > remaining()) {
            return truncated();
        }

        const auto* begin =
            reinterpret_cast<const char*>(bytes_.data() + position_);
        std::string value{begin, static_cast<std::size_t>(*size)};
        position_ += *size;
        return value;
    }

    [[nodiscard]] std::expected<std::optional<std::filesystem::path>,
                                ProtocolError>
    optional_path() {
        auto present = integer8();

        if (!present) {
            return std::unexpected{std::move(present.error())};
        }
        if (*present > 1) {
            return std::unexpected{error(ProtocolOperation::decode,
                                         "optional flag is not boolean")};
        }
        if (*present == 0) {
            return std::optional<std::filesystem::path>{};
        }

        auto value = text();

        if (!value) {
            return std::unexpected{std::move(value.error())};
        }

        return std::optional<std::filesystem::path>{
            std::filesystem::path{std::move(*value)}};
    }

    [[nodiscard]] std::expected<std::optional<std::string>, ProtocolError>
    optional_text() {
        auto present = integer8();

        if (!present) {
            return std::unexpected{std::move(present.error())};
        }
        if (*present > 1) {
            return std::unexpected{error(ProtocolOperation::decode,
                                         "optional flag is not boolean")};
        }
        if (*present == 0) {
            return std::optional<std::string>{};
        }

        auto value = text();

        if (!value) {
            return std::unexpected{std::move(value.error())};
        }

        return std::optional<std::string>{std::move(*value)};
    }

    [[nodiscard]] std::size_t remaining() const {
        return bytes_.size() - position_;
    }

  private:
    [[nodiscard]] std::unexpected<ProtocolError> truncated() const {
        return std::unexpected{
            error(ProtocolOperation::decode, "control frame is truncated")};
    }

    const std::vector<std::byte>& bytes_;
    std::size_t position_{0};
};

[[nodiscard]] std::expected<void, ProtocolError>
encode_job_spec(Writer& writer, const JobSpec& spec) {
    // cap counts separately from total bytes so a tiny packet cannot claim it
    // contains four billion arguments and waste the daemon's afternoon
    if (spec.argv.size() > max_collection_size ||
        spec.environment.size() > max_collection_size) {
        return std::unexpected{
            error(ProtocolOperation::encode, "job collection is too large")};
    }

    if (auto written = writer.text(spec.name); !written) {
        return written;
    }
    if (auto written = writer.text(spec.queue); !written) {
        return written;
    }

    writer.integer32(spec.resources.cpus);
    writer.integer64(spec.resources.memory_mb);
    writer.integer32(spec.resources.gpus);
    writer.integer32(static_cast<std::uint32_t>(spec.argv.size()));

    for (const auto& argument : spec.argv) {
        if (auto written = writer.text(argument); !written) {
            return written;
        }
    }

    if (auto written = writer.text(spec.working_directory.string()); !written) {
        return written;
    }

    writer.integer32(static_cast<std::uint32_t>(spec.environment.size()));

    for (const auto& variable : spec.environment) {
        if (auto written = writer.text(variable.name); !written) {
            return written;
        }
        if (auto written = writer.text(variable.value); !written) {
            return written;
        }
    }

    writer.integer8(spec.inherit_environment ? 1 : 0);

    if (auto written = writer.optional_text(spec.stdout_path); !written) {
        return written;
    }
    if (auto written = writer.optional_text(spec.stderr_path); !written) {
        return written;
    }

    writer.integer8(spec.append_output ? 1 : 0);
    writer.integer8(spec.walltime ? 1 : 0);

    if (spec.walltime) {
        writer.integer64(static_cast<std::uint64_t>(spec.walltime->count()));
    }

    return {};
}

[[nodiscard]] std::expected<JobSpec, ProtocolError>
decode_job_spec(Reader& reader) {
    // decode into temporary values first. jobspec only exists after every field
    // passed its bounds and boolean checks, never as a half-decoded mystery
    auto name = reader.text();
    auto queue = reader.text();
    auto cpus = reader.integer32();
    auto memory_mb = reader.integer64();
    auto gpus = reader.integer32();
    auto argument_count = reader.integer32();

    if (!name) {
        return std::unexpected{std::move(name.error())};
    }
    if (!queue) {
        return std::unexpected{std::move(queue.error())};
    }
    if (!cpus) {
        return std::unexpected{std::move(cpus.error())};
    }
    if (!memory_mb) {
        return std::unexpected{std::move(memory_mb.error())};
    }
    if (!gpus) {
        return std::unexpected{std::move(gpus.error())};
    }
    if (!argument_count) {
        return std::unexpected{std::move(argument_count.error())};
    }
    if (*argument_count > max_collection_size) {
        return std::unexpected{
            error(ProtocolOperation::decode, "argument list is too large")};
    }

    std::vector<std::string> arguments;
    arguments.reserve(*argument_count);

    for (std::uint32_t index = 0; index < *argument_count; ++index) {
        auto argument = reader.text();

        if (!argument) {
            return std::unexpected{std::move(argument.error())};
        }

        arguments.push_back(std::move(*argument));
    }

    auto working_directory = reader.text();
    auto environment_count = reader.integer32();

    if (!working_directory) {
        return std::unexpected{std::move(working_directory.error())};
    }
    if (!environment_count) {
        return std::unexpected{std::move(environment_count.error())};
    }
    if (*environment_count > max_collection_size) {
        return std::unexpected{
            error(ProtocolOperation::decode, "environment is too large")};
    }

    std::vector<EnvironmentVariable> environment;
    environment.reserve(*environment_count);

    for (std::uint32_t index = 0; index < *environment_count; ++index) {
        auto variable_name = reader.text();
        auto variable_value = reader.text();

        if (!variable_name) {
            return std::unexpected{std::move(variable_name.error())};
        }
        if (!variable_value) {
            return std::unexpected{std::move(variable_value.error())};
        }

        environment.push_back({
            .name = std::move(*variable_name),
            .value = std::move(*variable_value),
        });
    }

    auto inherit_environment = reader.integer8();

    if (!inherit_environment) {
        return std::unexpected{std::move(inherit_environment.error())};
    }
    if (*inherit_environment > 1) {
        return std::unexpected{
            error(ProtocolOperation::decode, "inherit flag is not boolean")};
    }

    auto stdout_path = reader.optional_path();
    auto stderr_path = reader.optional_path();
    auto append_output = reader.integer8();

    if (!stdout_path) {
        return std::unexpected{std::move(stdout_path.error())};
    }
    if (!stderr_path) {
        return std::unexpected{std::move(stderr_path.error())};
    }
    if (!append_output) {
        return std::unexpected{std::move(append_output.error())};
    }
    if (*append_output > 1) {
        return std::unexpected{
            error(ProtocolOperation::decode, "append flag is not boolean")};
    }

    auto has_walltime = reader.integer8();

    if (!has_walltime) {
        return std::unexpected{std::move(has_walltime.error())};
    }
    if (*has_walltime > 1) {
        return std::unexpected{
            error(ProtocolOperation::decode, "walltime flag is not boolean")};
    }

    std::optional<std::chrono::seconds> walltime;

    if (*has_walltime != 0) {
        auto seconds = reader.integer64();

        if (!seconds) {
            return std::unexpected{std::move(seconds.error())};
        }
        if (*seconds == 0 ||
            *seconds >
                static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max())) {
            return std::unexpected{
                error(ProtocolOperation::decode, "walltime is out of range")};
        }

        walltime = std::chrono::seconds{
            static_cast<std::chrono::seconds::rep>(*seconds)};
    }

    return JobSpec{
        .name = std::move(*name),
        .resources =
            {
                .cpus = *cpus,
                .memory_mb = *memory_mb,
                .gpus = *gpus,
            },
        .argv = std::move(arguments),
        .working_directory = std::move(*working_directory),
        .environment = std::move(environment),
        .inherit_environment = *inherit_environment != 0,
        .stdout_path = std::move(*stdout_path),
        .stderr_path = std::move(*stderr_path),
        .append_output = *append_output != 0,
        .walltime = walltime,
        .queue = std::move(*queue),
    };
}

void encode_job_state(Writer& writer, JobState state) {
    writer.integer8(static_cast<std::uint8_t>(state));
}

[[nodiscard]] std::expected<JobState, ProtocolError>
decode_job_state(Reader& reader) {
    auto encoded = reader.integer8();

    if (!encoded) {
        return std::unexpected{std::move(encoded.error())};
    }
    if (*encoded > static_cast<std::uint8_t>(JobState::cancelled)) {
        return std::unexpected{
            error(ProtocolOperation::decode, "job state is unknown")};
    }

    return static_cast<JobState>(*encoded);
}

void encode_optional_integer(Writer& writer, const std::optional<int>& value) {
    writer.integer8(value ? 1 : 0);

    if (value) {
        writer.integer32(static_cast<std::uint32_t>(*value));
    }
}

[[nodiscard]] std::expected<std::optional<int>, ProtocolError>
decode_optional_integer(Reader& reader) {
    auto present = reader.integer8();

    if (!present) {
        return std::unexpected{std::move(present.error())};
    }
    if (*present > 1) {
        return std::unexpected{
            error(ProtocolOperation::decode, "optional flag is not boolean")};
    }
    if (*present == 0) {
        return std::optional<int>{};
    }

    auto value = reader.integer32();

    if (!value) {
        return std::unexpected{std::move(value.error())};
    }
    if (*value > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
        return std::unexpected{
            error(ProtocolOperation::decode, "process result is out of range")};
    }

    return std::optional<int>{static_cast<int>(*value)};
}

void encode_job_result(Writer& writer, const std::optional<JobResult>& result) {
    writer.integer8(result ? 1 : 0);

    if (!result) {
        return;
    }

    encode_optional_integer(writer, result->exit_code);
    encode_optional_integer(writer, result->terminating_signal);
    writer.integer8(result->dumped_core ? 1 : 0);
}

[[nodiscard]] std::expected<std::optional<JobResult>, ProtocolError>
decode_job_result(Reader& reader) {
    auto present = reader.integer8();

    if (!present) {
        return std::unexpected{std::move(present.error())};
    }
    if (*present > 1) {
        return std::unexpected{
            error(ProtocolOperation::decode, "result flag is not boolean")};
    }
    if (*present == 0) {
        return std::optional<JobResult>{};
    }

    auto exit_code = decode_optional_integer(reader);
    auto signal = decode_optional_integer(reader);
    auto dumped_core = reader.integer8();

    if (!exit_code) {
        return std::unexpected{std::move(exit_code.error())};
    }
    if (!signal) {
        return std::unexpected{std::move(signal.error())};
    }
    if (!dumped_core) {
        return std::unexpected{std::move(dumped_core.error())};
    }
    if (*dumped_core > 1) {
        return std::unexpected{
            error(ProtocolOperation::decode, "core dump flag is not boolean")};
    }

    return std::optional<JobResult>{JobResult{
        .exit_code = std::move(*exit_code),
        .terminating_signal = std::move(*signal),
        .dumped_core = *dumped_core != 0,
    }};
}

void encode_optional_duration(
    Writer& writer, const std::optional<std::chrono::seconds>& duration) {
    writer.integer8(duration ? 1 : 0);

    if (duration) {
        writer.integer64(static_cast<std::uint64_t>(duration->count()));
    }
}

[[nodiscard]] std::expected<std::optional<std::chrono::seconds>, ProtocolError>
decode_optional_duration(Reader& reader) {
    auto present = reader.integer8();

    if (!present) {
        return std::unexpected{std::move(present.error())};
    }
    if (*present > 1) {
        return std::unexpected{
            error(ProtocolOperation::decode, "duration flag is not boolean")};
    }
    if (*present == 0) {
        return std::optional<std::chrono::seconds>{};
    }

    auto seconds = reader.integer64();

    if (!seconds) {
        return std::unexpected{std::move(seconds.error())};
    }
    if (*seconds >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return std::unexpected{
            error(ProtocolOperation::decode, "duration is out of range")};
    }

    return std::optional<std::chrono::seconds>{std::chrono::seconds{
        static_cast<std::chrono::seconds::rep>(*seconds)}};
}

void encode_job_owner(Writer& writer, const std::optional<JobOwner>& owner) {
    writer.integer8(owner ? 1 : 0);

    if (owner) {
        writer.integer32(owner->user_id);
        writer.integer32(owner->group_id);
    }
}

[[nodiscard]] std::expected<std::optional<JobOwner>, ProtocolError>
decode_job_owner(Reader& reader) {
    auto present = reader.integer8();

    if (!present) {
        return std::unexpected{std::move(present.error())};
    }
    if (*present > 1) {
        return std::unexpected{
            error(ProtocolOperation::decode,
                  "job owner flag is not boolean")};
    }
    if (*present == 0) {
        return std::optional<JobOwner>{};
    }

    auto user_id = reader.integer32();
    auto group_id = reader.integer32();

    if (!user_id) {
        return std::unexpected{std::move(user_id.error())};
    }
    if (!group_id) {
        return std::unexpected{std::move(group_id.error())};
    }

    return std::optional<JobOwner>{JobOwner{
        .user_id = *user_id,
        .group_id = *group_id,
    }};
}

[[nodiscard]] std::expected<void, ProtocolError>
encode_batch_queue(Writer& writer, const BatchQueue& queue) {
    if (auto written = writer.text(queue.name); !written) {
        return written;
    }

    const auto signed_priority = static_cast<std::int32_t>(queue.priority);
    writer.integer32(std::bit_cast<std::uint32_t>(signed_priority));
    writer.integer8(queue.enabled ? 1 : 0);
    writer.integer8(queue.started ? 1 : 0);
    writer.integer8(queue.max_running ? 1 : 0);

    if (queue.max_running) {
        writer.integer32(*queue.max_running);
    }

    return {};
}

[[nodiscard]] std::expected<BatchQueue, ProtocolError>
decode_batch_queue(Reader& reader) {
    auto name = reader.text();
    auto raw_priority = reader.integer32();
    auto enabled = reader.integer8();
    auto started = reader.integer8();
    auto has_max_running = reader.integer8();

    if (!name) {
        return std::unexpected{std::move(name.error())};
    }
    if (!raw_priority) {
        return std::unexpected{std::move(raw_priority.error())};
    }
    if (!enabled) {
        return std::unexpected{std::move(enabled.error())};
    }
    if (!started) {
        return std::unexpected{std::move(started.error())};
    }
    if (!has_max_running) {
        return std::unexpected{std::move(has_max_running.error())};
    }
    if (*enabled > 1 || *started > 1 || *has_max_running > 1) {
        return std::unexpected{
            error(ProtocolOperation::decode,
                  "queue flags are not boolean")};
    }

    std::optional<std::uint32_t> max_running;

    if (*has_max_running != 0) {
        auto limit = reader.integer32();

        if (!limit) {
            return std::unexpected{std::move(limit.error())};
        }
        if (*limit == 0) {
            return std::unexpected{
                error(ProtocolOperation::decode,
                      "queue max_running must be greater than zero")};
        }

        max_running = *limit;
    }

    return BatchQueue{
        .name = std::move(*name),
        .priority = static_cast<int>(
            std::bit_cast<std::int32_t>(*raw_priority)),
        .enabled = *enabled != 0,
        .started = *started != 0,
        .max_running = max_running,
    };
}

[[nodiscard]] std::expected<void, ProtocolError>
encode_job_summary(Writer& writer, const JobSummary& job) {
    writer.integer64(job.id);

    if (auto written = writer.text(job.name); !written) {
        return written;
    }
    if (auto written = writer.text(job.queue); !written) {
        return written;
    }

    encode_job_state(writer, job.state);
    writer.integer32(job.resources.cpus);
    writer.integer64(job.resources.memory_mb);
    writer.integer32(job.resources.gpus);
    if (auto written = writer.optional_text(job.assigned_node); !written) {
        return written;
    }

    encode_optional_duration(writer, job.walltime);
    encode_optional_duration(writer, job.execution_time);
    encode_job_owner(writer, job.owner);
    return {};
}

[[nodiscard]] std::expected<JobSummary, ProtocolError>
decode_job_summary(Reader& reader) {
    auto id = reader.integer64();
    auto name = reader.text();
    auto queue = reader.text();
    auto state = decode_job_state(reader);
    auto cpus = reader.integer32();
    auto memory_mb = reader.integer64();
    auto gpus = reader.integer32();
    auto assigned_node = reader.optional_text();
    auto walltime = decode_optional_duration(reader);
    auto execution_time = decode_optional_duration(reader);
    auto owner = decode_job_owner(reader);

    if (!id) {
        return std::unexpected{std::move(id.error())};
    }
    if (!name) {
        return std::unexpected{std::move(name.error())};
    }
    if (!queue) {
        return std::unexpected{std::move(queue.error())};
    }
    if (!state) {
        return std::unexpected{std::move(state.error())};
    }
    if (!cpus) {
        return std::unexpected{std::move(cpus.error())};
    }
    if (!memory_mb) {
        return std::unexpected{std::move(memory_mb.error())};
    }
    if (!gpus) {
        return std::unexpected{std::move(gpus.error())};
    }
    if (!assigned_node) {
        return std::unexpected{std::move(assigned_node.error())};
    }
    if (!walltime) {
        return std::unexpected{std::move(walltime.error())};
    }
    if (!execution_time) {
        return std::unexpected{std::move(execution_time.error())};
    }
    if (!owner) {
        return std::unexpected{std::move(owner.error())};
    }

    return JobSummary{
        .id = *id,
        .name = std::move(*name),
        .queue = std::move(*queue),
        .state = *state,
        .resources =
            {
                .cpus = *cpus,
                .memory_mb = *memory_mb,
                .gpus = *gpus,
            },
        .assigned_node = std::move(*assigned_node),
        .walltime = std::move(*walltime),
        .execution_time = std::move(*execution_time),
        .owner = std::move(*owner),
    };
}

[[nodiscard]] std::expected<void, ProtocolError> encode_job(Writer& writer,
                                                            const Job& job) {
    writer.integer64(job.id);
    writer.integer64(job.queue_sequence);

    if (auto encoded = encode_job_spec(writer, job.spec); !encoded) {
        return encoded;
    }

    encode_job_state(writer, job.state);

    if (auto encoded = writer.optional_text(job.assigned_node); !encoded) {
        return encoded;
    }

    encode_job_result(writer, job.result);
    encode_optional_duration(writer, job.execution_time);
    encode_job_owner(writer, job.owner);
    return {};
}

[[nodiscard]] std::expected<Job, ProtocolError> decode_job(Reader& reader) {
    auto id = reader.integer64();
    auto queue_sequence = reader.integer64();
    auto spec = decode_job_spec(reader);
    auto state = decode_job_state(reader);
    auto assigned_node = reader.optional_text();
    auto result = decode_job_result(reader);
    auto execution_time = decode_optional_duration(reader);
    auto owner = decode_job_owner(reader);

    if (!id) {
        return std::unexpected{std::move(id.error())};
    }
    if (!queue_sequence) {
        return std::unexpected{std::move(queue_sequence.error())};
    }
    if (!spec) {
        return std::unexpected{std::move(spec.error())};
    }
    if (!state) {
        return std::unexpected{std::move(state.error())};
    }
    if (!assigned_node) {
        return std::unexpected{std::move(assigned_node.error())};
    }
    if (!result) {
        return std::unexpected{std::move(result.error())};
    }
    if (!execution_time) {
        return std::unexpected{std::move(execution_time.error())};
    }
    if (!owner) {
        return std::unexpected{std::move(owner.error())};
    }

    return Job{
        .id = *id,
        .queue_sequence = *queue_sequence,
        .spec = std::move(*spec),
        .state = *state,
        .assigned_node = std::move(*assigned_node),
        .result = std::move(*result),
        .execution_time = std::move(*execution_time),
        .owner = std::move(*owner),
    };
}

void encode_capacity(Writer& writer, const ResourceCapacity& capacity) {
    writer.integer32(capacity.cpus);
    writer.integer64(capacity.memory_mb);
    writer.integer32(capacity.gpus);
}

[[nodiscard]] std::expected<ResourceCapacity, ProtocolError>
decode_capacity(Reader& reader) {
    auto cpus = reader.integer32();
    auto memory_mb = reader.integer64();
    auto gpus = reader.integer32();

    if (!cpus) {
        return std::unexpected{std::move(cpus.error())};
    }
    if (!memory_mb) {
        return std::unexpected{std::move(memory_mb.error())};
    }
    if (!gpus) {
        return std::unexpected{std::move(gpus.error())};
    }

    return ResourceCapacity{
        .cpus = *cpus,
        .memory_mb = *memory_mb,
        .gpus = *gpus,
    };
}

void encode_node_state(Writer& writer, NodeState state) {
    writer.integer8(static_cast<std::uint8_t>(state));
}

[[nodiscard]] std::expected<NodeState, ProtocolError>
decode_node_state(Reader& reader) {
    auto encoded = reader.integer8();

    if (!encoded) {
        return std::unexpected{std::move(encoded.error())};
    }
    if (*encoded > static_cast<std::uint8_t>(NodeState::offline)) {
        return std::unexpected{
            error(ProtocolOperation::decode, "node state is unknown")};
    }

    return static_cast<NodeState>(*encoded);
}

[[nodiscard]] std::expected<void, ProtocolError>
encode_node_summary(Writer& writer, const NodeSummary& node) {
    if (auto written = writer.text(node.id); !written) {
        return written;
    }

    encode_node_state(writer, node.state);
    encode_capacity(writer, node.total);
    encode_capacity(writer, node.reserved);
    encode_capacity(writer, node.allocated);
    encode_capacity(writer, node.available);
    return {};
}

[[nodiscard]] std::expected<NodeSummary, ProtocolError>
decode_node_summary(Reader& reader) {
    auto id = reader.text();
    auto state = decode_node_state(reader);
    auto total = decode_capacity(reader);
    auto reserved = decode_capacity(reader);
    auto allocated = decode_capacity(reader);
    auto available = decode_capacity(reader);

    if (!id) {
        return std::unexpected{std::move(id.error())};
    }
    if (!state) {
        return std::unexpected{std::move(state.error())};
    }
    if (!total) {
        return std::unexpected{std::move(total.error())};
    }
    if (!reserved) {
        return std::unexpected{std::move(reserved.error())};
    }
    if (!allocated) {
        return std::unexpected{std::move(allocated.error())};
    }
    if (!available) {
        return std::unexpected{std::move(available.error())};
    }

    return NodeSummary{
        .id = std::move(*id),
        .state = *state,
        .total = *total,
        .reserved = *reserved,
        .allocated = *allocated,
        .available = *available,
    };
}

[[nodiscard]] std::expected<std::vector<std::byte>, ProtocolError>
finish_frame(Writer payload) {
    // the first four bytes describe the payload length. seqpacket gives us a
    // boundary today, but tcp is only a byte stream and will need this later
    if (payload.bytes().size() >
        max_control_frame_size - sizeof(std::uint32_t)) {
        return std::unexpected{
            error(ProtocolOperation::encode, "control frame is too large")};
    }

    Writer frame;
    frame.integer32(static_cast<std::uint32_t>(payload.bytes().size()));
    frame.append(payload.bytes());
    return frame.take();
}

[[nodiscard]] std::expected<std::vector<std::byte>, ProtocolError>
payload_from_frame(const std::vector<std::byte>& frame) {
    // require the advertised length to match exactly. accepting "at least this
    // much" would let junk trail a valid request and complicate every upgrade
    if (frame.size() > max_control_frame_size) {
        return std::unexpected{
            error(ProtocolOperation::decode, "control frame is too large")};
    }

    Reader framed{frame};
    auto payload_size = framed.integer32();

    if (!payload_size) {
        return std::unexpected{std::move(payload_size.error())};
    }
    if (*payload_size != framed.remaining()) {
        return std::unexpected{
            error(ProtocolOperation::decode, "control frame size is wrong")};
    }

    return std::vector<std::byte>{
        frame.begin() + static_cast<std::ptrdiff_t>(sizeof(std::uint32_t)),
        frame.end(),
    };
}

[[nodiscard]] std::expected<std::uint8_t, ProtocolError>
read_header(Reader& reader) {
    // magic rejects random bytes, version rejects formats we do not understand,
    // and type says which body follows. guessing any of these would be cute
    auto magic = reader.integer32();
    auto version = reader.integer16();
    auto type = reader.integer8();

    if (!magic) {
        return std::unexpected{std::move(magic.error())};
    }
    if (!version) {
        return std::unexpected{std::move(version.error())};
    }
    if (!type) {
        return std::unexpected{std::move(type.error())};
    }
    if (*magic != protocol_magic) {
        return std::unexpected{
            error(ProtocolOperation::decode, "control frame magic is wrong")};
    }
    if (*version != protocol_version) {
        return std::unexpected{
            error(ProtocolOperation::decode,
                  "control protocol version is unsupported")};
    }

    return *type;
}

void write_header(Writer& writer, std::uint8_t type) {
    writer.integer32(protocol_magic);
    writer.integer16(protocol_version);
    writer.integer8(type);
}

} // namespace

std::expected<std::vector<std::byte>, ProtocolError>
encode_request(const ControlRequest& request) {
    Writer payload;

    if (const auto* submit = std::get_if<SubmitRequest>(&request)) {
        write_header(payload, submit_request_type);

        if (auto encoded = encode_job_spec(payload, submit->spec); !encoded) {
            return std::unexpected{std::move(encoded.error())};
        }
    } else if (std::holds_alternative<QueueRequest>(request)) {
        write_header(payload, queue_request_type);
    } else if (const auto* status = std::get_if<StatusRequest>(&request)) {
        write_header(payload, status_request_type);
        payload.integer64(status->job_id);
    } else if (const auto* cancel = std::get_if<CancelRequest>(&request)) {
        write_header(payload, cancel_request_type);
        payload.integer64(cancel->job_id);
    } else if (std::holds_alternative<NodesRequest>(request)) {
        write_header(payload, nodes_request_type);
    } else if (std::holds_alternative<QueuesRequest>(request)) {
        write_header(payload, queues_request_type);
    } else if (const auto* add = std::get_if<AddQueueRequest>(&request)) {
        write_header(payload, add_queue_request_type);

        if (auto encoded = encode_batch_queue(payload, add->queue); !encoded) {
            return std::unexpected{std::move(encoded.error())};
        }
    } else {
        const auto& update = std::get<UpdateQueueRequest>(request);
        write_header(payload, update_queue_request_type);

        if (auto written = payload.text(update.name); !written) {
            return std::unexpected{std::move(written.error())};
        }

        payload.integer8(static_cast<std::uint8_t>(update.action));
    }

    return finish_frame(std::move(payload));
}

std::expected<ControlRequest, ProtocolError>
decode_request(const std::vector<std::byte>& frame) {
    auto payload = payload_from_frame(frame);

    if (!payload) {
        return std::unexpected{std::move(payload.error())};
    }

    Reader reader{*payload};
    auto type = read_header(reader);

    if (!type) {
        return std::unexpected{std::move(type.error())};
    }
    ControlRequest request;

    if (*type == submit_request_type) {
        auto spec = decode_job_spec(reader);

        if (!spec) {
            return std::unexpected{std::move(spec.error())};
        }

        request = SubmitRequest{.spec = std::move(*spec)};
    } else if (*type == queue_request_type) {
        request = QueueRequest{};
    } else if (*type == status_request_type) {
        auto job_id = reader.integer64();

        if (!job_id) {
            return std::unexpected{std::move(job_id.error())};
        }

        request = StatusRequest{.job_id = *job_id};
    } else if (*type == cancel_request_type) {
        auto job_id = reader.integer64();

        if (!job_id) {
            return std::unexpected{std::move(job_id.error())};
        }

        request = CancelRequest{.job_id = *job_id};
    } else if (*type == nodes_request_type) {
        request = NodesRequest{};
    } else if (*type == queues_request_type) {
        request = QueuesRequest{};
    } else if (*type == add_queue_request_type) {
        auto queue = decode_batch_queue(reader);

        if (!queue) {
            return std::unexpected{std::move(queue.error())};
        }

        request = AddQueueRequest{.queue = std::move(*queue)};
    } else if (*type == update_queue_request_type) {
        auto name = reader.text();
        auto action = reader.integer8();

        if (!name) {
            return std::unexpected{std::move(name.error())};
        }
        if (!action) {
            return std::unexpected{std::move(action.error())};
        }
        if (*action > static_cast<std::uint8_t>(QueueAction::disable)) {
            return std::unexpected{
                error(ProtocolOperation::decode,
                      "queue action is unknown")};
        }

        request = UpdateQueueRequest{
            .name = std::move(*name),
            .action = static_cast<QueueAction>(*action),
        };
    } else {
        return std::unexpected{
            error(ProtocolOperation::decode, "unknown control request type")};
    }

    if (reader.remaining() != 0) {
        return std::unexpected{
            error(ProtocolOperation::decode, "control request has extra data")};
    }

    return request;
}

std::expected<std::vector<std::byte>, ProtocolError>
encode_response(const ControlResponse& response) {
    Writer payload;

    if (const auto* submitted = std::get_if<SubmitResponse>(&response)) {
        write_header(payload, submit_response_type);
        payload.integer64(submitted->job_id);
    } else if (const auto* queue = std::get_if<QueueResponse>(&response)) {
        if (queue->jobs.size() > max_collection_size) {
            return std::unexpected{
                error(ProtocolOperation::encode, "queue is too large")};
        }

        write_header(payload, queue_response_type);
        payload.integer32(static_cast<std::uint32_t>(queue->jobs.size()));

        for (const auto& job : queue->jobs) {
            if (auto encoded = encode_job_summary(payload, job); !encoded) {
                return std::unexpected{std::move(encoded.error())};
            }
        }
    } else if (const auto* status = std::get_if<StatusResponse>(&response)) {
        write_header(payload, status_response_type);

        if (auto encoded = encode_job(payload, status->job); !encoded) {
            return std::unexpected{std::move(encoded.error())};
        }
    } else if (const auto* cancelled = std::get_if<CancelResponse>(&response)) {
        write_header(payload, cancel_response_type);
        payload.integer64(cancelled->job_id);
    } else if (const auto* nodes = std::get_if<NodesResponse>(&response)) {
        if (nodes->nodes.size() > max_collection_size) {
            return std::unexpected{
                error(ProtocolOperation::encode, "node list is too large")};
        }

        write_header(payload, nodes_response_type);
        payload.integer32(static_cast<std::uint32_t>(nodes->nodes.size()));

        for (const auto& node : nodes->nodes) {
            if (auto encoded = encode_node_summary(payload, node); !encoded) {
                return std::unexpected{std::move(encoded.error())};
            }
        }
    } else if (const auto* queues = std::get_if<QueuesResponse>(&response)) {
        if (queues->queues.size() > max_collection_size) {
            return std::unexpected{
                error(ProtocolOperation::encode, "queue list is too large")};
        }

        write_header(payload, queues_response_type);
        payload.integer32(static_cast<std::uint32_t>(queues->queues.size()));

        for (const auto& queue : queues->queues) {
            if (auto encoded = encode_batch_queue(payload, queue); !encoded) {
                return std::unexpected{std::move(encoded.error())};
            }
        }
    } else if (const auto* updated =
                   std::get_if<QueueUpdatedResponse>(&response)) {
        write_header(payload, queue_updated_response_type);

        if (auto encoded = encode_batch_queue(payload, updated->queue);
            !encoded) {
            return std::unexpected{std::move(encoded.error())};
        }
    } else {
        write_header(payload, error_response_type);

        if (auto encoded =
                payload.text(std::get<ErrorResponse>(response).message);
            !encoded) {
            return std::unexpected{std::move(encoded.error())};
        }
    }

    return finish_frame(std::move(payload));
}

std::expected<ControlResponse, ProtocolError>
decode_response(const std::vector<std::byte>& frame) {
    auto payload = payload_from_frame(frame);

    if (!payload) {
        return std::unexpected{std::move(payload.error())};
    }

    Reader reader{*payload};
    auto type = read_header(reader);

    if (!type) {
        return std::unexpected{std::move(type.error())};
    }

    ControlResponse response;

    if (*type == submit_response_type) {
        auto job_id = reader.integer64();

        if (!job_id) {
            return std::unexpected{std::move(job_id.error())};
        }

        response = SubmitResponse{.job_id = *job_id};
    } else if (*type == queue_response_type) {
        auto count = reader.integer32();

        if (!count) {
            return std::unexpected{std::move(count.error())};
        }
        if (*count > max_collection_size) {
            return std::unexpected{
                error(ProtocolOperation::decode, "queue is too large")};
        }

        std::vector<JobSummary> jobs;
        jobs.reserve(*count);

        for (std::uint32_t index = 0; index < *count; ++index) {
            auto job = decode_job_summary(reader);

            if (!job) {
                return std::unexpected{std::move(job.error())};
            }

            jobs.push_back(std::move(*job));
        }

        response = QueueResponse{.jobs = std::move(jobs)};
    } else if (*type == status_response_type) {
        auto job = decode_job(reader);

        if (!job) {
            return std::unexpected{std::move(job.error())};
        }

        response = StatusResponse{.job = std::move(*job)};
    } else if (*type == cancel_response_type) {
        auto job_id = reader.integer64();

        if (!job_id) {
            return std::unexpected{std::move(job_id.error())};
        }

        response = CancelResponse{.job_id = *job_id};
    } else if (*type == nodes_response_type) {
        auto count = reader.integer32();

        if (!count) {
            return std::unexpected{std::move(count.error())};
        }
        if (*count > max_collection_size) {
            return std::unexpected{
                error(ProtocolOperation::decode, "node list is too large")};
        }

        std::vector<NodeSummary> nodes;
        nodes.reserve(*count);

        for (std::uint32_t index = 0; index < *count; ++index) {
            auto node = decode_node_summary(reader);

            if (!node) {
                return std::unexpected{std::move(node.error())};
            }

            nodes.push_back(std::move(*node));
        }

        response = NodesResponse{.nodes = std::move(nodes)};
    } else if (*type == queues_response_type) {
        auto count = reader.integer32();

        if (!count) {
            return std::unexpected{std::move(count.error())};
        }
        if (*count > max_collection_size) {
            return std::unexpected{
                error(ProtocolOperation::decode,
                      "queue list is too large")};
        }

        std::vector<BatchQueue> queues;
        queues.reserve(*count);

        for (std::uint32_t index = 0; index < *count; ++index) {
            auto queue = decode_batch_queue(reader);

            if (!queue) {
                return std::unexpected{std::move(queue.error())};
            }

            queues.push_back(std::move(*queue));
        }

        response = QueuesResponse{.queues = std::move(queues)};
    } else if (*type == queue_updated_response_type) {
        auto queue = decode_batch_queue(reader);

        if (!queue) {
            return std::unexpected{std::move(queue.error())};
        }

        response = QueueUpdatedResponse{.queue = std::move(*queue)};
    } else if (*type == error_response_type) {
        auto message = reader.text();

        if (!message) {
            return std::unexpected{std::move(message.error())};
        }

        response = ErrorResponse{.message = std::move(*message)};
    } else {
        return std::unexpected{
            error(ProtocolOperation::decode, "unknown control response type")};
    }

    if (reader.remaining() != 0) {
        return std::unexpected{error(ProtocolOperation::decode,
                                     "control response has extra data")};
    }

    return response;
}

} // namespace rlbs
