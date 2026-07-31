// resolving an account is normal parent-process work; changing credentials is
// tiny child-process work. this split is the whole point of this file.
#include <rlbs/execution/identity.hpp>

#include <cerrno>
#include <cstdint>
#include <grp.h>
#include <pwd.h>
#include <string_view>
#include <utility>

#include <unistd.h>

namespace rlbs {
namespace {

[[nodiscard]] std::string identity_label(JobOwner owner) {
    return "uid=" + std::to_string(owner.user_id) +
           " gid=" + std::to_string(owner.group_id);
}

[[nodiscard]] IdentityError error(IdentityOperation operation,
                                  int system_error, std::string message) {
    return {
        .operation = operation,
        .system_error = system_error,
        .message = std::move(message),
    };
}

[[nodiscard]] std::expected<std::string, IdentityError>
username_for(uid_t user_id, JobOwner owner) {
    // getpwuid_r needs caller-owned scratch storage. 16k handles normal passwd
    // entries; erange grows it without trusting one fixed libc-specific size.
    std::vector<char> buffer(16 * 1024);

    for (;;) {
        passwd record{};
        passwd* found = nullptr;
        const int result = ::getpwuid_r(user_id, &record, buffer.data(),
                                        buffer.size(), &found);

        if (result == 0 && found != nullptr && found->pw_name != nullptr) {
            return std::string{found->pw_name};
        }
        if (result == 0) {
            return std::unexpected{
                error(IdentityOperation::resolve_account, ESRCH,
                      identity_label(owner) + " has no passwd entry")};
        }
        if (result != ERANGE ||
            buffer.size() >= static_cast<std::size_t>(1024 * 1024)) {
            return std::unexpected{
                error(IdentityOperation::resolve_account, result,
                      "could not resolve " + identity_label(owner))};
        }

        buffer.resize(buffer.size() * 2);
    }
}

[[nodiscard]] std::expected<std::vector<gid_t>, IdentityError>
groups_for(std::string_view username, gid_t primary_group, JobOwner owner) {
    // getgrouplist tells us the required count by failing the first undersized
    // call. starting with a useful guess avoids making failure the common path.
    std::vector<gid_t> groups(16);
    int count = static_cast<int>(groups.size());
    std::string owned_username{username};

    if (::getgrouplist(owned_username.c_str(), primary_group, groups.data(),
                       &count) < 0) {
        const long configured_limit = ::sysconf(_SC_NGROUPS_MAX);
        const long group_limit =
            configured_limit > 0 ? configured_limit : 65536;

        // nss supplies this count. cap it to what the kernel can actually use
        // before an absurd response turns into an absurd allocation.
        if (count <= 0 || static_cast<long>(count) > group_limit) {
            return std::unexpected{
                error(IdentityOperation::resolve_groups, EOVERFLOW,
                      "group list is unreasonable for " +
                          identity_label(owner))};
        }

        groups.resize(static_cast<std::size_t>(count));

        if (::getgrouplist(owned_username.c_str(), primary_group,
                           groups.data(), &count) < 0) {
            return std::unexpected{
                error(IdentityOperation::resolve_groups, EIO,
                      "could not resolve groups for " +
                          identity_label(owner))};
        }
    }

    groups.resize(static_cast<std::size_t>(count));
    return groups;
}

} // namespace

std::expected<PreparedIdentity, IdentityError>
prepare_identity(JobOwner requested) {
    const auto uid = static_cast<uid_t>(requested.user_id);
    const auto gid = static_cast<gid_t>(requested.group_id);

    if (static_cast<std::uintmax_t>(uid) != requested.user_id ||
        static_cast<std::uintmax_t>(gid) != requested.group_id) {
        return std::unexpected{
            error(IdentityOperation::resolve_account, EOVERFLOW,
                  identity_label(requested) + " does not fit this system")};
    }

    const auto current_uid = ::geteuid();
    const auto current_gid = ::getegid();

    // a private single-user daemon is already the requested identity and
    // already has that account's supplementary groups. no privilege needed.
    if (uid == current_uid && gid == current_gid) {
        return PreparedIdentity{
            .user_id = uid,
            .group_id = gid,
            .supplementary_groups = {},
            .switch_credentials = false,
        };
    }

    // a normal process cannot become another uid. rejecting before fork gives
    // callers a useful error instead of a half-completed child setup.
    if (current_uid != 0) {
        return std::unexpected{
            error(IdentityOperation::resolve_account, EPERM,
                  "rlbsd cannot switch from uid=" +
                      std::to_string(current_uid) + " to " +
                      identity_label(requested))};
    }

    auto username = username_for(uid, requested);

    if (!username) {
        return std::unexpected{std::move(username.error())};
    }

    auto groups = groups_for(*username, gid, requested);

    if (!groups) {
        return std::unexpected{std::move(groups.error())};
    }

    return PreparedIdentity{
        .user_id = uid,
        .group_id = gid,
        .supplementary_groups = std::move(*groups),
        .switch_credentials = true,
    };
}

std::optional<IdentitySwitchFailure>
switch_identity(const PreparedIdentity& identity) {
    if (!identity.switch_credentials) {
        return std::nullopt;
    }

    const auto* groups = identity.supplementary_groups.empty()
                             ? nullptr
                             : identity.supplementary_groups.data();

    // groups go first. changing uid first would remove the privilege needed by
    // both group calls and leave the job carrying daemon access.
    if (::setgroups(identity.supplementary_groups.size(), groups) < 0) {
        return IdentitySwitchFailure{
            .operation = IdentityOperation::set_supplementary_groups,
            .system_error = errno,
        };
    }
    if (::setgid(identity.group_id) < 0) {
        return IdentitySwitchFailure{
            .operation = IdentityOperation::set_group_id,
            .system_error = errno,
        };
    }
    if (::setuid(identity.user_id) < 0) {
        return IdentitySwitchFailure{
            .operation = IdentityOperation::set_user_id,
            .system_error = errno,
        };
    }

    // success should mean success, but checking costs nothing and catches odd
    // platform semantics before user code gets anywhere near exec.
    if (::geteuid() != identity.user_id) {
        return IdentitySwitchFailure{
            .operation = IdentityOperation::set_user_id,
            .system_error = EACCES,
        };
    }
    if (::getegid() != identity.group_id) {
        return IdentitySwitchFailure{
            .operation = IdentityOperation::set_group_id,
            .system_error = EACCES,
        };
    }

    // if a non-root child can get uid 0 back, saved credentials survived and
    // the drop was fake. the caller must abort this child immediately.
    if (identity.user_id != 0 && ::setuid(0) == 0) {
        return IdentitySwitchFailure{
            .operation = IdentityOperation::set_user_id,
            .system_error = EACCES,
        };
    }

    return std::nullopt;
}

} // namespace rlbs
