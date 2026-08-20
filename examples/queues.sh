#!/usr/bin/env bash

# this is the small queue tour, not a production installer. start rlbsd first,
# run it once, then poke at the commands below without having to remember which
# switch means "accept jobs" and which one means "actually launch them".
set -eu

# override these if the build or socket lives somewhere else:
#   RLBS_BIN=/opt/rlbs/bin/rlbs RLBS_SOCKET=/run/rlbs/rlbs.sock ./queues.sh
rlbs_bin="${RLBS_BIN:-./build/rlbs}"
rlbs_socket="${RLBS_SOCKET:-/run/rlbs/rlbs.sock}"

# the seeded default queue is already here. this prints every queue and its two
# switches before we change anything.
"$rlbs_bin" queues --socket "$rlbs_socket"

# higher priority wins between queues. max-running=2 means this queue can have
# two assigned/starting/running jobs before its next job has to wait.
"$rlbs_bin" queues add short \
    --priority 100 \
    --max-running 2 \
    --socket "$rlbs_socket"

# submit normally; --queue is the only extra bit jobs need to care about.
"$rlbs_bin" submit \
    --queue short \
    --name queue-demo \
    --socket "$rlbs_socket" \
    -- /bin/sh -c 'echo "hello from the short queue"'

# stop keeps accepting new jobs but leaves them pending. useful when the queue
# policy is fine and the execution side needs a minute.
# "$rlbs_bin" queues stop short --socket "$rlbs_socket"
# "$rlbs_bin" queues start short --socket "$rlbs_socket"

# disable rejects new submissions. jobs already waiting are still allowed to
# run unless the queue is also stopped.
# "$rlbs_bin" queues disable short --socket "$rlbs_socket"
# "$rlbs_bin" queues enable short --socket "$rlbs_socket"

"$rlbs_bin" queues --socket "$rlbs_socket"
"$rlbs_bin" queue --socket "$rlbs_socket"
