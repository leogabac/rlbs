#!/usr/bin/env bash

# rlbsd reads uid/gid from the unix socket, stores them on the job, and runs the
# execution child as that account. there is still no user option because job
# text should never get a vote in which unix identity executes it.
set -eu

rlbs_bin="${RLBS_BIN:-./build/rlbs}"
rlbs_socket="${RLBS_SOCKET:-/run/rlbs/rlbs.sock}"

printf 'shell identity: %s:%s\n' "$(id -u)" "$(id -g)"

# there is intentionally no --uid or --user option here. if the client could
# choose those values, ownership would be decorative instead of trustworthy.
submission="$(
    "$rlbs_bin" submit \
        --socket "$rlbs_socket" \
        --name ownership-demo \
        -- /bin/sh -c 'echo "ownership demo ran"'
)"

# native submit prints "submitted job N", so the final word is the job id.
job_id="${submission##* }"
printf '%s\n' "$submission"

# owner uid:gid should match the shell identity printed above, and the command
# itself runs with that identity too.
"$rlbs_bin" status --socket "$rlbs_socket" "$job_id"
