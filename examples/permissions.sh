#!/usr/bin/env bash

# rlbsd trusts the uid attached to the unix socket by the kernel. job owners
# can see and cancel their own jobs; root and the account running rlbsd can see
# everything and change queues. execution children drop to the recorded owner.
set -eu

rlbs_bin="${RLBS_BIN:-./build/rlbs}"
rlbs_socket="${RLBS_SOCKET:-/tmp/rlbs.sock}"

printf 'current client uid: %s\n' "$(id -u)"
"$rlbs_bin" queue --socket "$rlbs_socket"

# a real two-user check needs both users in one scheduler group. start the
# daemon with --socket-group rlbs-users; it keeps the socket at 0660 and changes
# only its group owner. these are commented because nobody wants an example
# script unexpectedly creating users or invoking sudo.
#
# sudo ./build/rlbsd --database /var/lib/rlbs/rlbs.db \
#     --socket /run/rlbs/rlbs.sock --socket-group rlbs-users --cpus 8
#
# sudo -u alice "$rlbs_bin" submit --socket "$rlbs_socket" \
#     --name alice-job -- /bin/sh -c 'sleep 60'
#
# alice will see alice-job, while bob's queue output will leave it out:
# sudo -u alice "$rlbs_bin" queue --socket "$rlbs_socket"
# sudo -u bob "$rlbs_bin" queue --socket "$rlbs_socket"
#
# bob cannot inspect or cancel alice's job:
# sudo -u bob "$rlbs_bin" status --socket "$rlbs_socket" JOB_ID
# sudo -u bob "$rlbs_bin" cancel --socket "$rlbs_socket" JOB_ID
#
# queue definitions are readable, but changing them is administrator-only:
# sudo -u bob "$rlbs_bin" queues --socket "$rlbs_socket"
# sudo -u bob "$rlbs_bin" queues stop default --socket "$rlbs_socket"
# sudo "$rlbs_bin" queues stop default --socket "$rlbs_socket"

# a system-wide daemon normally starts as root so it can switch to each owner.
# a private non-root daemon safely runs jobs from its own uid, but rejects jobs
# owned by anyone else instead of accidentally running them as the daemon.
#
# final stdout/stderr publishing also runs as the owner. output files start as
# mode 0600, and -o/-e cannot use root to sneak into an unwritable directory.
