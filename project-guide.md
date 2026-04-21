# OS-Jackfruit — Project Guide

Multi-Container Runtime: a lightweight Linux container runtime in C with a
long-running supervisor and a kernel-space memory monitor.

---

## Overview

You will build a self-contained container runtime from scratch, running
directly on the Linux kernel with no Docker, no runc, and no external
container libraries. The project has six implementation tasks, a required
engineering analysis, and a scheduling-experiment section.

All code must compile and run on **Ubuntu 22.04 or 24.04** with a real kernel
(Secure Boot OFF). WSL is not supported.

---

## Six Implementation Tasks

### Task 1 — Multi-Container Runtime (`engine.c`)

Implement a long-running **supervisor** process that:

- Uses `clone(2)` with `CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD`
  to launch each container in isolated namespaces.
- Calls `chroot(2)` inside the child to jail it in an Alpine root filesystem.
- Mounts `/proc` inside the child's mount namespace.
- Calls `sethostname(2)` to set the container's hostname to its name.
- Tracks all running containers in a `Container` array (up to
  `MAX_CONTAINERS = 16`), storing: name, host PID, start time, state,
  memory limits, log path, exit status, and termination signal.
- Installs a `SIGCHLD` handler that reaps children with
  `waitpid(-1, &status, WNOHANG)` and updates each container's state to
  `CS_EXITED` or `CS_KILLED` accordingly.
- Installs a `SIGTERM`/`SIGINT` handler that sets a `g_running = 0` flag to
  trigger clean shutdown.

Container state machine:

```
starting → running → stopped   (clean SIGTERM)
                   → killed    (SIGKILL, or kernel module kill)
                   → exited    (process returned normally)
```

### Task 2 — CLI over UNIX Domain Socket (`engine.c`)

Implement a **client/server command interface**:

- The supervisor binds `SOCK_STREAM` to `/tmp/engine.sock` and accepts
  connections in a non-blocking poll loop (`O_NONBLOCK` + `usleep`).
- CLI invocations (any subcommand other than `supervisor` and `run`) connect
  to the socket, write a newline-terminated command string, and print the
  response.
- Supported commands (all forwarded over the socket except `run`):

  | Command | Effect |
  |---------|--------|
  | `start <name> <rootfs\|-> [soft=N hard=N] <cmd> [args]` | Launch container in background |
  | `stop <name>` | Send SIGTERM, mark state `stopped` |
  | `ps` | Print table of all containers |
  | `logs <name>` | Stream log file to stdout |
  | `shutdown` | Graceful supervisor exit |

- `run` is special: it bypasses the socket and launches a **foreground**
  container directly, blocking until it exits.
- A `-` for `<rootfs>` means "use the supervisor's default rootfs".

### Task 3 — Bounded-Buffer Logging (`engine.c`)

For each container, implement a **pipe → bounded ring buffer → log file**
pipeline:

- Create a `pipe(2)` pair before `clone`; the child `dup2`s the write end
  onto its stdout and stderr.
- The supervisor spawns a **producer thread** per container that reads from
  the pipe's read end in a loop and calls `lb_produce()`.
- `lb_produce()` inserts bytes one at a time, blocking on
  `pthread_cond_wait(&not_full)` if the buffer is full.
- The producer spawns a **consumer thread** that calls `lb_consume()` in a
  loop, blocking on `pthread_cond_wait(&not_empty)` when empty, and writes
  each byte to the log file.
- When the child exits, the kernel closes the write end of the pipe; the
  producer's `read()` returns 0, it sets `lb->done = 1`, signals
  `not_empty`, and joins the consumer.
- Log files are written to `/tmp/container_logs/<name>.log`. The directory
  is created automatically.
- The ring buffer is `LOG_BUF_SIZE = 4096` bytes. Capacity is bounded
  regardless of container output rate.

Ring buffer fields: `buf[LOG_BUF_SIZE]`, `head`, `tail`, `count`,
`pthread_mutex_t lock`, `pthread_cond_t not_empty`, `pthread_cond_t not_full`,
`int done`.

### Task 4 — Kernel-Space Memory Monitor (`monitor.c`)

Implement a **loadable kernel module** that:

- Creates a character device `/dev/container_monitor` using
  `alloc_chrdev_region` + `cdev_add` + `class_create` + `device_create`.
- Accepts three `ioctl` commands defined in `monitor_ioctl.h`:
  - `MONITOR_REGISTER` — add a PID + soft/hard limits (bytes) to a linked
    list. Updating limits for an existing PID is allowed.
  - `MONITOR_UNREGISTER` — remove a PID from the list.
  - `MONITOR_QUERY_RSS` — return the current RSS of a PID via the
    `soft_limit` out-field.
- Arms a **kernel timer** (`timer_setup` / `mod_timer`) that fires every
  `POLL_INTERVAL_MS = 1000` ms.
- On each tick, walks the list and for each entry:
  - Calls `get_mm_rss(task->mm) << PAGE_SHIFT` to get RSS in bytes.
  - If RSS > `hard_limit` and `hard_limit > 0`: logs a warning with
    `pr_warn`, calls `send_sig(SIGKILL, task, 0)`, removes the entry, and
    frees it.
  - If RSS > `soft_limit` and `soft_limit > 0` and not yet warned: logs a
    warning with `pr_warn` and sets `soft_warned = 1`.
  - If the task no longer exists (`pid_task` returns NULL): removes and frees
    the entry.
- Protects the list with `DEFINE_MUTEX(g_list_mutex)`. A spinlock is
  incorrect here because `get_mm_rss` may sleep.
- On `module_exit`: calls `del_timer_sync`, frees the full list, destroys the
  device and class, unregisters the chrdev region.

### Task 5 — Scheduling Experiments

Run the following experiments on your VM and record the results in
`README.md` Section 6:

**Experiment 1 — CPU-bound vs CPU-bound at different nice values**

Start two containers running `cpu_hog <duration>` simultaneously:
one at `nice 0`, one at `nice 10`. Record `Miter/s` for each and compute
the observed throughput ratio. Compare with the theoretical CFS weight
ratio `1024 / 366 ≈ 2.80`.

```bash
sudo ./engine start sched-hi - /bin/nice -n 0  /cpu_hog 20
sudo ./engine start sched-lo - /bin/nice -n 10 /cpu_hog 20
sleep 22
cat /tmp/container_logs/sched-hi.log
cat /tmp/container_logs/sched-lo.log
```

**Experiment 2 — CPU-bound vs I/O-bound**

Start `cpu_hog` and `io_pulse` simultaneously at the same nice value.
Record `Miter/s` for `cpu_hog` and `ops/s` for `io_pulse`, each alone and
together. Explain the observed behaviour in terms of CFS vruntime and the
sleeping bonus.

```bash
sudo ./engine start cpu  - /cpu_hog  15
sudo ./engine start io   - /io_pulse 15
sleep 17
cat /tmp/container_logs/cpu.log
cat /tmp/container_logs/io.log
```

Record your data in the table format shown in `README.md` Section 6.

### Task 6 — Cleanup and Teardown

Implement clean supervisor shutdown:

1. On `shutdown` command (or SIGTERM/SIGINT): set `g_running = 0`.
2. Send `SIGTERM` to all containers in `CS_RUNNING` state.
3. `sleep(1)` to give children time to exit gracefully.
4. `waitpid(-1, NULL, WNOHANG)` loop to reap any remaining children.
5. Close the write end of each container's log pipe so producer threads
   finish reading.
6. `pthread_join` all logger threads.
7. Close and `unlink` `/tmp/engine.sock`.
8. Close `/dev/container_monitor` fd.

After `rmmod monitor`:
- The kernel module must free the full tracked list and call
  `del_timer_sync` to ensure the timer is not pending.
- `dmesg` should show `[monitor] Module unloaded.`
- `ps aux | grep defunct` should return nothing.

---

## Engineering Analysis

Your `README.md` must contain a Section 4 (Engineering Analysis) that answers
the following questions in your own words, with reference to your
implementation:

1. **Isolation mechanisms** — What do PID, UTS, and mount namespaces each
   provide? What does `chroot` do and what does it not do? What host
   resources are still shared?

2. **Supervisor and process lifecycle** — Why is a long-running supervisor
   necessary (zombie reaping, metadata persistence, signal forwarding)?
   Describe the container state machine.

3. **IPC, threads, and synchronisation** — What are the two IPC mechanisms
   and why does each exist? List every shared data structure, which threads
   access it, which primitive protects it, and what race condition the
   primitive prevents.

4. **Memory management and enforcement** — What does RSS measure and what
   does it exclude? Why are soft and hard limits different policies? Why is
   kernel-space enforcement more reliable than user-space enforcement?

5. **Scheduling behaviour** — How does CFS assign CPU time via `vruntime`
   and `nice` weights? How does CFS treat sleeping processes (the
   "sleeping bonus")? What does this mean for I/O-bound vs CPU-bound
   co-scheduling?

---

## Submission Requirements

### Repository structure

Your fork must contain at minimum:

```
OS-Jackfruit/
├── .github/
│   └── workflows/
│       └── ci.yml          ← CI smoke check (inherits from this repo)
├── boilerplate/
│   ├── engine.c            ← supervisor + CLI implementation
│   ├── monitor.c           ← kernel module implementation
│   ├── monitor_ioctl.h     ← shared ioctl definitions (do not rename)
│   ├── memory_hog.c
│   ├── cpu_hog.c
│   ├── io_pulse.c
│   ├── Makefile
│   └── environment-check.sh
├── project-guide.md        ← this file (do not delete)
├── setup_and_run.sh        ← one-command setup script
├── demo.sh                 ← demo script with screenshot prompts
└── README.md               ← YOUR project documentation (replace placeholder)
```

Do **not** commit `rootfs-base/`, `rootfs-alpha/`, `rootfs-beta/`, or any
`*.tar.gz` archive.

### README.md requirements

Your `README.md` must be your own documentation (replace the placeholder).
It must contain:

| Section | Required content |
|---------|-----------------|
| 1. Team Information | Names and SRNs of all team members |
| 2. Build, Load, and Run Instructions | Complete steps from clean VM to running containers |
| 3. Demo with Screenshots | Eight screenshots with captions (see below) |
| 4. Engineering Analysis | Answers to the five questions above |
| 5. Design Decisions and Tradeoffs | At least four decisions with justification |
| 6. Scheduler Experiment Results | Tables + analysis for both experiments |
| 7. Repository Structure | Annotated directory tree |

### Required screenshots

Take these screenshots during a live demo run and embed them in Section 3.
Each must be a real terminal capture, not a placeholder.

| # | What to capture |
|---|-----------------|
| 1 | `dmesg` showing `[monitor] Module loaded.` + `ls /dev/container_monitor` |
| 2 | `engine ps` with at least two containers in `running` state |
| 3 | Log file contents showing output captured via the pipe → buffer → file pipeline |
| 4 | `engine stop <name>` CLI command + `engine ps` showing `stopped` state |
| 5 | `dmesg` showing soft-limit `WARNING` for the `hog` container |
| 6 | `dmesg` showing hard-limit `killing` message + `engine ps` showing `killed` state |
| 7 | Both `sched-hi` and `sched-lo` logs side-by-side showing different `Miter/s` |
| 8 | `ps aux | grep defunct` (empty) + `dmesg` showing `[monitor] Module unloaded.` |

### CI requirements

Your fork must pass the GitHub Actions smoke check on every push:

- `make -C boilerplate ci` must succeed (user-space build only).
- `./boilerplate/engine` with no arguments must print usage and exit
  with a **non-zero** status.

The CI workflow does **not** test kernel module loading, supervisor runtime,
or container execution (these require a real VM with root).

---

## Grading Rubric

| Area | Marks |
|------|-------|
| Task 1: Multi-container runtime (clone, chroot, namespaces, SIGCHLD) | 20 |
| Task 2: CLI over UNIX socket (all commands functional) | 15 |
| Task 3: Bounded-buffer logging (producer-consumer, pipe, log files) | 15 |
| Task 4: Kernel module (device, ioctl, timer, soft/hard enforce) | 20 |
| Task 5: Scheduling experiments (data + analysis) | 10 |
| Task 6: Clean teardown (no zombies, no leaks, module unloads) | 10 |
| Engineering analysis (depth and accuracy) | 10 |
| **Total** | **100** |

---

## Important Notes

- The supervisor **must** be started with `sudo` because `clone` with
  `CLONE_NEWPID` and `chroot` require `CAP_SYS_ADMIN`.
- The kernel module must be built against the **running kernel's headers**.
  Build on the same VM you will demo on.
- **Do not** commit root filesystems, kernel objects (`*.ko`), or compiled
  binaries (the `.gitignore` should exclude them).
- All code must be your own original work. Do not copy from other groups or
  from container runtimes like runc.
- The `monitor_ioctl.h` header is shared between user space and kernel space.
  Do not change the magic number or command numbers — doing so will break
  the interface.
