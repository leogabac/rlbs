// job launch and output publishing both cross the same unix identity boundary.
// keeping the ugly credential rules here means those two paths cannot slowly
// develop different opinions about which groups should survive.
#pragma once

#include <expected>
#include <optional>
#include <string>
#include <vector>

#include <sys/types.h>

#include <rlbs/core/job_owner.hpp>

namespace rlbs {

enum class IdentityOperation {
    resolve_account,
    resolve_groups,
    set_supplementary_groups,
    set_group_id,
    set_user_id,
};

struct IdentityError {
    IdentityOperation operation{IdentityOperation::resolve_account};
    int system_error{0};
    std::string message;
};

// everything here is prepared before fork. the child gets plain ids and an
// already-built group list, so it does not ask libc/nss complicated questions
// while it is living in that awkward pre-exec half-process state.
struct PreparedIdentity {
    uid_t user_id{0};
    gid_t group_id{0};
    std::vector<gid_t> supplementary_groups;
    bool switch_credentials{false};
};

struct IdentitySwitchFailure {
    IdentityOperation operation{IdentityOperation::set_user_id};
    int system_error{0};
};

[[nodiscard]] std::expected<PreparedIdentity, IdentityError>
prepare_identity(JobOwner requested);

// this only performs small credential syscalls and returns a fixed-size error,
// which makes it safe to call from the child side of fork.
[[nodiscard]] std::optional<IdentitySwitchFailure>
switch_identity(const PreparedIdentity& identity);

} // namespace rlbs
