// authorization lives below the command handlers on purpose. the handler
// should ask one boring question and move on, instead of scattering uid checks
// through socket code until one of them inevitably behaves differently.
#include <rlbs/control/authorization.hpp>

namespace rlbs {

AuthorizationPolicy::AuthorizationPolicy(JobOwner daemon_owner)
    : daemon_owner_{daemon_owner} {}

bool AuthorizationPolicy::is_administrator(JobOwner requester) const {
    // uid 0 is the kernel's privileged identity. the daemon uid is also an
    // admin because a user running a private rlbsd should own that scheduler.
    // gid is deliberately ignored: primary groups change and are not identity.
    return requester.user_id == 0 ||
           requester.user_id == daemon_owner_.user_id;
}

bool AuthorizationPolicy::can_read_job(JobOwner requester,
                                       const Job& job) const {
    if (is_administrator(requester)) {
        return true;
    }

    // schema upgrades left older jobs without an owner. there is no safe way
    // to reconstruct one, so only an administrator gets to inspect those.
    return job.owner &&
           job.owner->user_id == requester.user_id;
}

bool AuthorizationPolicy::can_cancel_job(JobOwner requester,
                                         const Job& job) const {
    // reading and cancelling share the same ownership rule for now. keeping a
    // separate method makes the distinction explicit if policy grows later.
    return can_read_job(requester, job);
}

bool AuthorizationPolicy::can_manage_queues(JobOwner requester) const {
    // queues affect everybody using the daemon, so job ownership is nowhere
    // near enough authority to stop one or disable submissions for everyone.
    return is_administrator(requester);
}

} // namespace rlbs
