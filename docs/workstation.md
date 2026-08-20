# RLBS single-node workstation setup

This is the small, useful deployment: one workstation runs `rlbsd` as a system
service and local users submit work through the installed `rlbs`, `qsub`,
`qstat`, and `qdel` commands. It does not need networking or a separate head
node.

`rlbsd` runs as root so its child process can become the user who submitted the
job. The job itself, its environment, and published stdout/stderr run as that
user; root is not inherited by submitted commands.

## Install

Build and install a release. The commands below use `/usr/local`; choose a
different CMake prefix before configuring if that is where your workstation
keeps locally built software.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build build -j"$(nproc)"
sudo cmake --install build
```

The install provides these commands in `/usr/local/bin`:

- `rlbsd` — the daemon
- `rlbs` — native command-line interface
- `qsub`, `qstat`, `qdel` — PBS-style aliases to `rlbs`

It also installs the service unit and an untouched configuration example:

```text
/usr/local/lib/systemd/system/rlbsd.service
/usr/local/share/rlbs/rlbsd.conf.example
```

## Create the local submitter group

The service exposes a Unix socket owned by `root:rlbs` and mode `0660`. Members
of `rlbs` may connect; the kernel tells the daemon the real uid/gid of each
client, so a user cannot submit a job as somebody else.

On a new workstation:

```bash
sudo groupadd --system rlbs
sudo usermod -aG rlbs alice
sudo usermod -aG rlbs bob
```

Replace `alice` and `bob` with actual submitter accounts. Each added user must
start a new login session before the new group appears in `id -nG`.

## Configure the node

Create the system configuration from the installed example:

```bash
sudo install -d -m 0755 /etc/rlbs
sudo install -m 0644 /usr/local/share/rlbs/rlbsd.conf.example /etc/rlbs/rlbsd.conf
sudoedit /etc/rlbs/rlbsd.conf
```

Set `node_id`, `cpus`, `memory_mb`, `gpus`, and the reserved resources for the
actual workstation. The important paths are already set up for the service:

```ini
database = /var/lib/rlbs/rlbs.db
socket = /run/rlbs/rlbs.sock
socket_group = rlbs
spool = /var/lib/rlbs/spool
```

`cpus` is the total capacity known to RLBS, not just the portion intended for
batch work. Keep interactive capacity with `reserve_cpus` and
`reserve_memory_mb`; the scheduler will not allocate that reserve.

The file format is deliberately plain `key = value`. Blank lines and `#`
comments are allowed. `rlbsd --config PATH` loads a file, and explicit daemon
arguments override its values for one-off diagnostics.

## Start the service

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now rlbsd
systemctl status rlbsd
journalctl -u rlbsd -f
```

The service creates `/run/rlbs` for the socket and `/var/lib/rlbs` for the
database/spool. Do not make users write to either directory directly.

On a normal stop, RLBS closes the submission socket and waits for existing jobs
to finish. Therefore `systemctl stop rlbsd` and `systemctl restart rlbsd` may
wait for active work; use `qdel JOB_ID` first when you need a quick shutdown.

## First user smoke test

Log in as a non-root account that belongs to `rlbs` and check the socket:

```bash
id -nG
ls -l /run/rlbs/rlbs.sock
```

The socket should show group `rlbs` and mode `srw-rw----`. Submit a short job:

```bash
rlbs submit \
    --name workstation-smoke \
    --cpus 1 \
    --stdout workstation-smoke.out \
    --stderr workstation-smoke.err \
    -- /bin/sh -c 'echo "uid=$(id -u) gid=$(id -g)"; sleep 2; echo done'
```

Then inspect it:

```bash
qstat
qstat -x
qstat JOB_ID
cat workstation-smoke.out
```

Plain `qstat` and `rlbs queue` show active work only. `qstat -x` or
`rlbs queue --all` also show completed, failed, and cancelled history. Jobs are
only visible to their owner, root, and the account running `rlbsd`.

Cancel an active job with either interface:

```bash
qdel JOB_ID
# or
rlbs cancel JOB_ID
```

## Queues and current permissions

Opening a database creates a started and enabled `default` queue. Root may
manage queue policy:

```bash
sudo rlbs queues
sudo rlbs queues add short --priority 100 --max-running 2
sudo rlbs queues stop short
sudo rlbs queues start short
```

At this MVP stage, membership in the `rlbs` Unix group grants socket access and
users may submit to available queues. Queue administration is limited to root
(or the daemon account for a private daemon); per-queue user ACLs are not yet
implemented. Do not treat the shared group as a per-queue permission system.

## Routine operations

```bash
# active jobs, then all historical jobs
qstat
qstat -x

# local resource accounting
rlbs nodes

# service logs and configuration check after an edit
journalctl -u rlbsd -n 100 --no-pager
sudo systemctl restart rlbsd
```

The database is durable at `/var/lib/rlbs/rlbs.db`; restarting the daemon does
not clear completed history. After an abrupt host crash, the next startup marks
stale `assigned`, `starting`, and `running` records as failed and records why
in their job history. Pending jobs remain queued. Do not `SIGKILL` a standalone
daemon while its jobs may still be alive; cancel those jobs or stop the service
cleanly first.
