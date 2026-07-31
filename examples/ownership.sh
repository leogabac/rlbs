#!/usr/bin/env bash

# this demonstrates recorded ownership, not user impersonation yet. rlbsd reads
# uid/gid from the unix socket and stores them on the job, but the execution
# child still runs as the daemon account until privilege dropping is added.
set -eu

rlbs_bin="${RLBS_BIN:-./build/rlbs}"
rlbs_socket="${RLBS_SOCKET:-/tmp/rlbs.sock}"

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

# owner uid:gid should match the shell identity printed above.
"$rlbs_bin" status --socket "$rlbs_socket" "$job_id"
