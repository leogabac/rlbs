// the socket gives us a trustworthy uid, but that fact is useless unless every
// command agrees on what the uid may do. this policy keeps those answers in one
// place so a new control command cannot invent its own exciting security model.
#pragma once

#include <rlbs/core/job.hpp>
#include <rlbs/core/job_owner.hpp>

namespace rlbs {

class AuthorizationPolicy {
  public:
    explicit AuthorizationPolicy(JobOwner daemon_owner);

    // root is always an administrator. the account running rlbsd is one too,
    // which keeps the normal single-user development setup usable without
    // pretending every client on the socket should control every job.
    [[nodiscard]] bool is_administrator(JobOwner requester) const;

    [[nodiscard]] bool can_read_job(JobOwner requester, const Job& job) const;
    [[nodiscard]] bool can_cancel_job(JobOwner requester,
                                      const Job& job) const;
    [[nodiscard]] bool can_manage_queues(JobOwner requester) const;

  private:
    JobOwner daemon_owner_;
};

} // namespace rlbs
