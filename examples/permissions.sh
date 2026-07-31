#!/usr/bin/env bash

# rlbsd trusts the uid attached to the unix socket by the kernel. job owners
# can see and cancel their own jobs; root and the account running rlbsd can see
# everything and change queues. jobs still execute as the daemon user for now.
set -eu

rlbs_bin="${RLBS_BIN:-./build/rlbs}"
rlbs_socket="${RLBS_SOCKET:-/tmp/rlbs.sock}"

printf 'current client uid: %s\n' "$(id -u)"
"$rlbs_bin" queue --socket "$rlbs_socket"

# a real two-user check needs both users to reach the socket. rlbsd creates it
# as 0660, so put the users in its group (or let the service manager assign the
# socket group) before trying this. these are commented because nobody wants an
# example script unexpectedly creating users or invoking sudo.
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
