// these tests pin the permission table without needing fake unix credentials
// or a root-only integration test. socket handling only supplies the identity;
// this is the bit that decides what that identity is allowed to touch.
#include <rlbs/control/authorization.hpp>

#include <iostream>
#include <string_view>

namespace {

int authorization_failures = 0;

void expect_authorized(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "failed: " << message << '\n';
        ++authorization_failures;
    }
}

[[nodiscard]] rlbs::Job owned_job(rlbs::JobOwner owner) {
    rlbs::Job job;
    job.id = 42;
    job.owner = owner;
    return job;
}

void test_administrators() {
    const rlbs::AuthorizationPolicy policy{
        rlbs::JobOwner{.user_id = 1000, .group_id = 100}};

    expect_authorized(
        policy.is_administrator({.user_id = 0, .group_id = 0}),
        "root is an administrator");
    expect_authorized(
        policy.is_administrator({.user_id = 1000, .group_id = 999}),
        "daemon uid is an administrator even if its group changed");
    expect_authorized(
        !policy.is_administrator({.user_id = 2000, .group_id = 100}),
        "sharing the daemon group does not make a user an administrator");
}

void test_job_permissions() {
    const rlbs::AuthorizationPolicy policy{
        rlbs::JobOwner{.user_id = 1000, .group_id = 100}};
    const auto job =
        owned_job(rlbs::JobOwner{.user_id = 2000, .group_id = 200});
    const rlbs::JobOwner owner{.user_id = 2000, .group_id = 999};
    const rlbs::JobOwner stranger{.user_id = 3000, .group_id = 200};
    const rlbs::JobOwner daemon{.user_id = 1000, .group_id = 100};

    expect_authorized(policy.can_read_job(owner, job),
                      "owner can inspect their job");
    expect_authorized(policy.can_cancel_job(owner, job),
                      "owner can cancel their job");
    expect_authorized(!policy.can_read_job(stranger, job),
                      "another user cannot inspect the job");
    expect_authorized(!policy.can_cancel_job(stranger, job),
                      "another user cannot cancel the job");
    expect_authorized(policy.can_read_job(daemon, job),
                      "daemon owner can inspect every job");
    expect_authorized(policy.can_cancel_job(daemon, job),
                      "daemon owner can cancel every job");
}

void test_legacy_jobs_are_admin_only() {
    const rlbs::AuthorizationPolicy policy{
        rlbs::JobOwner{.user_id = 1000, .group_id = 100}};
    rlbs::Job legacy;
    legacy.id = 7;

    expect_authorized(
        !policy.can_read_job({.user_id = 2000, .group_id = 200}, legacy),
        "ordinary user cannot inspect an unowned legacy job");
    expect_authorized(
        policy.can_read_job({.user_id = 1000, .group_id = 100}, legacy),
        "daemon owner can inspect an unowned legacy job");
}

void test_queue_administration() {
    const rlbs::AuthorizationPolicy policy{
        rlbs::JobOwner{.user_id = 1000, .group_id = 100}};

    expect_authorized(
        policy.can_manage_queues({.user_id = 0, .group_id = 0}),
        "root can manage queues");
    expect_authorized(
        policy.can_manage_queues({.user_id = 1000, .group_id = 100}),
        "daemon owner can manage queues");
    expect_authorized(
        !policy.can_manage_queues({.user_id = 2000, .group_id = 100}),
        "ordinary user cannot manage queues");
}

} // namespace

// protocol_test.cpp owns main for this combined test binary.
int run_authorization_tests() {
    test_administrators();
    test_job_permissions();
    test_legacy_jobs_are_admin_only();
    test_queue_administration();
    return authorization_failures;
}
