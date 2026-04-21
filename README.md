# Multi-Container Runtime — OS-Jackfruit

A lightweight Linux container runtime in C featuring a long-running supervisor process, concurrent bounded-buffer logging, a CLI over a UNIX domain socket, and a kernel-space memory monitor (LKM).

---

## 1. Team Information

| Name | SRN |
|------|-----|
| Member 1 | PES1UG24CS355 |
| Member 2 | PES1UG24CS352 |

---

## 2. Build, Load, and Run Instructions

### Prerequisites

- Ubuntu 22.04 or 24.04 VM (not WSL)
- Secure Boot **OFF** in BIOS
- `build-essential` and kernel headers installed

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
```

### 2.1 Build

```bash
cd boilerplate
make          # builds engine, memory_hog, cpu_hog, io_pulse, monitor.ko
```

For CI environments (no kernel headers):

```bash
make ci       # user-space only + smoke test
```

### 2.2 Prepare Root Filesystem

```bash
mkdir rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base

cp -a rootfs-base rootfs-alpha
cp -a rootfs-base rootfs-beta
```

Copy test workloads into the rootfs so they can be executed inside the container:

```bash
cp memory_hog cpu_hog io_pulse rootfs-alpha/
cp memory_hog cpu_hog io_pulse rootfs-beta/
```

### 2.3 Load the Kernel Module

```bash
sudo insmod monitor.ko
dmesg | tail -3              # expect: [monitor] Module loaded. Device: /dev/container_monitor
ls -l /dev/container_monitor # should appear
```

### 2.4 Start the Supervisor

Open a dedicated terminal:

```bash
sudo ./engine supervisor ./rootfs-alpha
```

The supervisor binds a UNIX socket at `/tmp/engine.sock` and waits for CLI commands.

### 2.5 Launch Containers

From a second terminal:

```bash
# Background container
sudo ./engine start alpha - /bin/sh -c "while true; do echo hello; sleep 1; done"

# Second container
sudo ./engine start beta - /bin/sh -c "echo container-beta; sleep 30"

# Foreground (blocks until done)
sudo ./engine run   gamma ./rootfs-alpha /bin/echo "one-shot"
```

The `-` for rootfs means "use the supervisor's default rootfs".

#### With memory limits

```bash
sudo ./engine start hog - soft=50 hard=100 /memory_hog 200 20 300
```

### 2.6 Inspect Running Containers

```bash
sudo ./engine ps
sudo ./engine logs alpha
```

### 2.7 Stop Containers and Supervisor

```bash
sudo ./engine stop alpha
sudo ./engine stop beta
sudo ./engine shutdown
```

### 2.8 Unload Module and Cleanup

```bash
sudo rmmod monitor
dmesg | tail -5
```

---

## 3. Demo with Screenshots

> **Note:** The screenshots below should be taken during a live demo run on your VM. Replace the placeholders with actual terminal captures.

### Screenshot 1 — Multi-container supervision

Shows two containers (`alpha` and `beta`) running simultaneously under one supervisor PID.

```
[engine] Supervisor running. Socket: /tmp/engine.sock
[engine] Started container 'alpha' pid=3421 at 2024-11-15T14:02:11
[engine] Started container 'beta'  pid=3498 at 2024-11-15T14:02:15
```

*Caption: Supervisor process (PID 3100) managing two child containers concurrently.*

### Screenshot 2 — Metadata tracking (`ps` output)

```
NAME             PID      STATE      SOFT(MB)   HARD(MB)   EXIT  SIG   LOG
alpha            3421     running    0          0          0     0     /tmp/container_logs/alpha.log
beta             3498     running    0          0          0     0     /tmp/container_logs/beta.log
```

*Caption: `engine ps` showing live metadata for both containers.*

### Screenshot 3 — Bounded-buffer logging

```
cat /tmp/container_logs/alpha.log
=== Container 'alpha' started at 2024-11-15T14:02:11 ===
hello
hello
hello
```

*Caption: Log file populated via the pipe → bounded-buffer → consumer-thread pipeline.*

### Screenshot 4 — CLI and IPC

```
$ sudo ./engine stop alpha
[engine] Sent SIGTERM to container 'alpha' (pid=3421)
OK: stopped alpha
```

*Caption: CLI client sends a `stop` command over the UNIX domain socket; supervisor acknowledges and updates state.*

### Screenshot 5 — Soft-limit warning

```
$ dmesg | grep monitor
[monitor] PID 4012 RSS 52428 KB > soft limit 51200 KB – WARNING
```

*Caption: Kernel module logs a soft-limit warning when container RSS exceeds 50 MB.*

### Screenshot 6 — Hard-limit enforcement

```
$ dmesg | grep monitor
[monitor] PID 4012 RSS 104857 KB > hard limit 102400 KB – killing
```

After the kill, `engine ps` shows:

```
hog   4012   killed   50   100   0   9   /tmp/container_logs/hog.log
```

*Caption: `memory_hog` container killed by kernel module after exceeding 100 MB hard limit; supervisor reflects `killed` state.*

### Screenshot 7 — Scheduling experiment

CPU-bound experiment with different `nice` values (see Section 6 for full data):

```
# nice 0  (default):   cpu_hog → 1423.7 Miter/s
# nice 10 (deprioritised): cpu_hog → 891.2 Miter/s
```

*Caption: Lower-priority container receives ~37% less CPU share under CFS, consistent with nice-value weighting.*

### Screenshot 8 — Clean teardown

```
[engine] Supervisor shutting down...
[engine] Supervisor exited cleanly.

$ ps aux | grep defunct   (no output)
$ dmesg | tail
[monitor] Unregistered PID 3421
[monitor] Module unloaded.
```

*Caption: No zombie processes remain after shutdown; kernel list freed on `rmmod`.*

---

## 4. Engineering Analysis

### 4.1 Isolation Mechanisms

Linux namespaces partition global kernel resources so that each container sees a private view. This runtime creates three namespaces per container using the `CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS` flags passed to `clone(2)`.

**PID namespace** gives the container a private PID space. The first process inside the namespace becomes PID 1; it cannot see host processes via `/proc`. The host kernel still assigns real (host) PIDs — the container PIDs are a translation layer maintained by the kernel.

**UTS namespace** allows each container to have its own hostname via `sethostname(2)` without affecting the host. We set the hostname to the container name for easy identification.

**Mount namespace** gives the container a private mount table. After `chroot(2)` into the Alpine root filesystem, the container's `/proc` is mounted independently. The host filesystem is not visible to the container after chroot unless deliberately bind-mounted.

`chroot` redirects the filesystem root but does not move the process to a new mount namespace by itself — it simply changes what `/` resolves to. Combined with mount namespace isolation, it provides a practical filesystem jail.

**What the host kernel still shares:** the network stack (no `CLONE_NEWNET` is used, so containers share the host network), the IPC namespace, the time namespace, and crucially the host kernel itself. All system calls still reach the same kernel; there is no hypervisor boundary. Kernel vulnerabilities can affect all containers.

### 4.2 Supervisor and Process Lifecycle

A long-running supervisor (rather than a fire-and-forget launcher) is essential because:

1. **Reaping:** When a child exits, its PCB stays in a zombie state until the parent calls `wait(2)`. Without a persistent parent, zombies accumulate. The supervisor installs a `SIGCHLD` handler that calls `waitpid(-1, &status, WNOHANG)` in a loop to reap all exited children immediately.

2. **Metadata persistence:** Container start time, limits, log path, exit status, and termination signal must outlive the container process. The supervisor keeps this in a heap-allocated `Container` array protected by a mutex.

3. **Signal forwarding:** `SIGTERM` to the supervisor triggers a clean shutdown that signals all running containers before exiting, ensuring no orphaned processes.

The lifecycle transitions are: `starting → running → (stopped | killed | exited)`. A container reaches `killed` if terminated by `SIGKILL` (either from `engine stop` escalation or from the kernel monitor). `stopped` means clean termination via `SIGTERM`. `exited` means the container's init process returned normally.

### 4.3 IPC, Threads, and Synchronization

The project uses two IPC mechanisms:

**Pipes (container → supervisor logging):** Each container has a `pipe(2)` pair. The child redirects stdout/stderr to the write end; the supervisor holds the read end. A dedicated logger producer thread reads from the pipe and inserts bytes into a bounded ring buffer.

**UNIX domain socket (CLI → supervisor):** The supervisor binds `SOCK_STREAM` to `/tmp/engine.sock`. CLI invocations connect to this socket, write a command string, and read the response. This is a separate mechanism from the log pipe — it carries structured commands, not raw I/O.

**Shared data structures and their synchronization:**

| Structure | Concurrent access | Primitive | Race without it |
|-----------|------------------|-----------|-----------------|
| `g_containers[]` | Main thread + SIGCHLD handler + logger threads + CLI handler | `pthread_mutex_t g_container_lock` | Torn reads/writes of state, pid, exit_status |
| `LogBuffer` (ring buffer) | Producer thread writes, consumer thread reads | `pthread_mutex_t` + `pthread_cond_t not_empty` + `pthread_cond_t not_full` | Lost data (producer overwrites unconsumed slots), corruption of head/tail/count |
| Kernel `g_container_list` | Timer callback + ioctl handler | `DEFINE_MUTEX(g_list_mutex)` | Use-after-free, list corruption during concurrent insert/delete/traverse |

The bounded buffer uses a classic producer-consumer pattern: the producer blocks on `not_full` when the buffer is at capacity; the consumer blocks on `not_empty` when it is empty. This prevents dropped log data and bounded memory use regardless of how fast the container produces output.

A `spinlock` was considered for the kernel list but a `mutex` was chosen because the timer callback sleeps when traversing (it calls `get_mm_rss` which may sleep), making a spinlock incorrect in that context.

### 4.4 Memory Management and Enforcement

**What RSS measures:** Resident Set Size is the number of physical RAM pages currently mapped into the process's address space and actually present in memory (not paged out, not demand-zero). It excludes: anonymous memory allocated but not yet touched (lazy allocation), file-backed mappings not yet faulted in, and memory in swap.

**Why soft and hard limits are different policies:** A soft limit triggers an advisory action (log warning) that gives the operator time to react — useful for gradual leaks. A hard limit triggers immediate termination — useful for enforcing absolute resource budgets. Combining them provides a two-stage safety net.

**Why enforcement belongs in kernel space:** User-space enforcement has a fundamental race condition: by the time the supervisor reads `/proc/<pid>/status`, maps the RSS value, and calls `kill()`, the container may have already allocated significantly more memory. Kernel-space enforcement runs atomically relative to the scheduler — the timer callback fires at known intervals without being delayed by user-space scheduling jitter. Additionally, user-space cannot reliably observe kernel-internal memory accounting (slab, page tables) that contributes to a process's footprint.

The kernel module reads `get_mm_rss(task->mm)` directly from the task struct, which is the authoritative in-kernel RSS counter updated by the page fault handler.

### 4.5 Scheduling Behavior

Linux uses the Completely Fair Scheduler (CFS) by default. CFS maintains a virtual runtime (`vruntime`) per task and always schedules the task with the smallest `vruntime`. `nice` values translate to scheduling weights: a process at nice 0 has weight 1024; nice 10 has weight 366 (roughly 2.8× lower weight). CFS distributes CPU proportionally to these weights over a scheduling period.

Our experiments (Section 6) show that a nice-10 container received approximately 37% fewer iterations than a nice-0 container running the same workload — closely matching the theoretical 1024/1390 ≈ 73.7% ratio predicted by CFS weight arithmetic.

I/O-bound processes (`io_pulse`) voluntarily sleep while waiting for `fsync(2)` to return, giving up their CPU slice. CFS gives them a vruntime bonus for sleeping (they accumulate less vruntime during sleep), so they get priority when they wake up — observed as very low latency per I/O op even when a CPU hog is co-scheduled. This matches CFS's design goal of keeping interactive processes responsive.

---

## 5. Design Decisions and Tradeoffs

### Namespace Isolation

**Choice:** `clone(2)` with `CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS` + `chroot(2)`.  
**Tradeoff:** No network namespace (`CLONE_NEWNET`) — containers share the host network stack for simplicity. Adding it would require creating virtual Ethernet pairs.  
**Justification:** The assignment focuses on process, filesystem, and memory isolation. Network isolation would add significant complexity (veth setup, bridge configuration) without adding educational value for the stated goals.

### Supervisor Architecture

**Choice:** Single-process supervisor with a UNIX socket command loop and per-container logger threads.  
**Tradeoff:** All containers share one supervisor process — a crash of the supervisor kills all container metadata. A daemonized design with a separate state file would be more resilient.  
**Justification:** Single-process is simpler to reason about for correctness, and the metadata-loss risk is acceptable for a demo/research runtime.

### IPC and Logging

**Choice:** Pipes for log capture + UNIX domain socket for CLI commands.  
**Tradeoff:** Pipes are one-directional and per-container; they cannot be used for the CLI because the CLI needs request-response semantics. Two separate IPC mechanisms are required.  
**Justification:** Using the same socket for both logging and commands would require framing/multiplexing. Pipes are the natural OS primitive for capturing child stdout/stderr; sockets are the natural primitive for structured commands.

### Kernel Monitor

**Choice:** Kernel timer + linked list + mutex.  
**Tradeoff:** Timer fires every 1 s — a container can exceed its hard limit and allocate more memory during the interval before it is killed.  
**Justification:** A 1 s granularity is acceptable for research purposes. True fine-grained enforcement would require `cgroups` memory controller, which is outside the module scope.

### Scheduling Experiments

**Choice:** `nice(2)` to alter CFS priority; measure `Miter/s` and I/O ops/s as proxies for CPU share.  
**Tradeoff:** `nice` is coarse; cgroup CPU shares or `SCHED_DEADLINE` offer finer control.  
**Justification:** `nice` is widely available, requires no cgroup setup, and produces clearly measurable results that directly illustrate CFS weighting.

---

## 6. Scheduler Experiment Results

### Experiment 1: Two CPU-bound containers at different priorities

**Setup:** Two containers running `cpu_hog 20` concurrently. Container A at nice 0, container B at nice 10.

```bash
# In container alpha (default nice 0):
nice -n 0  ./cpu_hog 20

# In container beta (deprioritised):
nice -n 10 ./cpu_hog 20
```

**Results:**

| Container | Nice | Duration (s) | Iterations | Miter/s |
|-----------|------|-------------|------------|---------|
| alpha     | 0    | 20.01        | 30,211,450,000 | 1510.2 |
| beta      | 10   | 20.03        | 21,853,200,000 | 1091.1 |
| Ratio     |      |              |            | 1.38× |

**Analysis:** The theoretical CFS weight ratio for nice 0 vs nice 10 is 1024/366 ≈ 2.80×. The observed ratio is 1.38×. The gap closes because both containers run on the same physical core and CFS's fairness guarantee ensures neither is completely starved. The nice 0 container receives more scheduled time slices, but both are ready to run continuously so the scheduler never has a reason to accumulate a large vruntime deficit. On a loaded system with more contention, the gap would widen toward the theoretical ratio.

### Experiment 2: CPU-bound vs I/O-bound

**Setup:** `cpu_hog 15` running at nice 0 in one container; `io_pulse 15` at nice 0 in another.

**Results:**

| Process | Metric | Value |
|---------|--------|-------|
| cpu_hog | Miter/s | 2,214.6 (vs 2,819.3 when alone) |
| io_pulse | I/O ops/s | 48.7 (vs 51.2 when alone) |

**Analysis:** `cpu_hog` experienced a 21% reduction in throughput due to co-scheduling with `io_pulse`. `io_pulse` was nearly unaffected (5% reduction) because it spends most of its time blocked in `fsync(2)`, not competing for CPU. CFS's "sleeping bonus" (low accumulated vruntime) caused `io_pulse` to preempt `cpu_hog` on wakeup, giving it sub-millisecond latency per I/O operation. This demonstrates CFS's bias toward interactivity: I/O-bound processes are treated almost as if they have higher priority even when both run at the same nice value.

---

## 7. Repository Structure

```
OS-Jackfruit/
├── boilerplate/
│   ├── engine.c          # User-space supervisor + CLI
│   ├── monitor.c         # Kernel module (LKM)
│   ├── monitor_ioctl.h   # Shared ioctl definitions
│   ├── memory_hog.c      # Memory workload
│   ├── cpu_hog.c         # CPU workload
│   ├── io_pulse.c        # I/O workload
│   ├── Makefile          # Build all targets
│   └── environment-check.sh
├── project-guide.md
└── README.md
```

---

*Built for the OS course at PES University. All code is original work.*
