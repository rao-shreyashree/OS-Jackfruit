# Multi-Container Runtime

A lightweight Linux container runtime in C — a stripped-down Docker-like system with a long-running supervisor, isolated containers, a logging pipeline, and a kernel-space memory monitor.

---

## Team

| Person | SRN | Tasks |
|--------|-----|-------|
| Shreyashree | PES1UG24AM267 | Task 1 - Multi-Container Runtime, Task 2 - CLI & Signal Handling, Task 3 - Bounded-Buffer Logging |
| Shravani | PES1UG24AM264 | Task 4 - Kernel Memory Monitor |
| Shriya | PES1UG24AM270 | Task 5 - Scheduler Experiments, Task 6 - Cleanup |

---

## Project Overview

The runtime is a single binary (`engine`) used two ways:

- **Supervisor daemon** — started once, stays alive, manages containers, owns the logging pipeline
- **CLI client** — short-lived process that sends commands to the supervisor over a UNIX socket

Two IPC paths:

- **Path A (logging):** container stdout/stderr → pipe → producer thread → bounded buffer → consumer thread → log file
- **Path B (control):** CLI process → UNIX domain socket → supervisor → response

---

## Files

| File | Purpose |
|------|---------|
| `boilerplate/engine.c` | User-space runtime: supervisor, CLI, logging pipeline |
| `boilerplate/monitor.c` | Kernel module: memory monitor with soft/hard limits |
| `boilerplate/monitor_ioctl.h` | Shared ioctl definitions between engine and kernel module |
| `boilerplate/cpu_hog.c` | CPU-bound workload for scheduler experiments |
| `boilerplate/io_pulse.c` | I/O-bound workload for scheduler experiments |
| `boilerplate/memory_hog.c` | Memory workload for kernel monitor testing |
| `boilerplate/run_experiments.sh` | Script to reproduce Task 5 scheduler experiments |
| `boilerplate/Makefile` | Builds engine, workloads, and kernel module |
| `scheduler_experiments.md` | Full Task 5 results and analysis |

---

## Environment Setup

Requires **Ubuntu 22.04 or 24.04** in a VM, **Secure Boot OFF**. Not compatible with WSL for kernel module tasks.

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
```

Run the preflight check:

```bash
cd boilerplate
chmod +x environment-check.sh
sudo ./environment-check.sh
```

Download Alpine rootfs and create per-container copies:

```bash
cd boilerplate
mkdir rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base
cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta
```

Build everything:

```bash
cd boilerplate
make
```

CI-safe build (user-space only, no kernel module):

```bash
make -C boilerplate ci
```

---

## Task 1 — Multi-Container Runtime

### Implementation

- Supervisor process stays alive and manages multiple containers concurrently
- Each container launched via `clone()` with `CLONE_NEWPID`, `CLONE_NEWUTS`, `CLONE_NEWNS` — isolated PID, UTS, and mount namespaces
- `chroot()` locks each container into its own rootfs directory
- `/proc` mounted inside each container so tools like `ps` work
- Per-container metadata tracked in a mutex-protected linked list (PID, state, limits, log path, exit code)
- `SIGCHLD` handler calls `waitpid()` in a loop to reap all exited children — no zombies

### Running

Terminal 1 — start the supervisor (leave it running):

```bash
sudo ./engine supervisor ./rootfs-base
```

Expected output: `[supervisor] listening on /tmp/mini_runtime.sock rootfs=./rootfs-base`

Terminal 2 — launch two containers and check metadata:

```bash
sudo ./engine start alpha ./rootfs-alpha "echo hello from alpha"
sudo ./engine start beta ./rootfs-beta "echo hello from beta"
sudo ./engine ps
```

`ps` prints a table of both containers with PID, state, memory limits, and log path.

---

## Task 2 — CLI and Signal Handling

### Implementation

- UNIX domain socket at `/tmp/mini_runtime.sock` connects CLI to supervisor
- CLI sends a `control_request_t` struct, reads back a `control_response_t`, prints and exits
- Commands: `start`, `run`, `ps`, `logs`, `stop`
- `stop` sends `SIGTERM` first, waits 5 seconds, then `SIGKILL`
- `stop_requested` flag set before signalling so `SIGCHLD` handler correctly classifies exit as `stopped` vs `killed` vs `hard_limit_killed`
- `SIGINT`/`SIGTERM` to supervisor triggers orderly shutdown — stops all containers, joins threads, cleans up socket

### Running

Stop a container and verify state change:

```bash
sudo ./engine start gamma ./rootfs-alpha "sleep 30"
sudo ./engine ps          # gamma shows: running
sudo ./engine stop gamma
sudo ./engine ps          # gamma shows: stopped
```

Run a container in the foreground and wait for it:

```bash
sudo ./engine run delta ./rootfs-alpha "echo hello from delta"
```

---

## Task 3 — Bounded-Buffer Logging

### Implementation

- Each container's stdout/stderr piped to a dedicated producer thread
- Producer pushes 4KB chunks into a shared bounded buffer (16 slots)
- Single consumer thread drains buffer and writes to `logs/<id>.log`
- Mutex + condition variables (`not_full`, `not_empty`) — threads sleep instead of spinning
- On shutdown, condvars are broadcast so threads wake, drain remaining data, and exit cleanly — no log lines lost

### Synchronisation Justification

A mutex protects the bounded buffer because producers and consumers run in process context (sleeping is allowed). Condition variables `not_full` and `not_empty` let threads block instead of busy-wait, which avoids wasting CPU. Without this synchronisation, a producer could overwrite a slot the consumer hasn't read yet (data corruption), or a consumer could read an empty slot (undefined behaviour). The bounded size (16 slots) prevents unbounded memory growth if containers produce output faster than the consumer can drain.

### Running

```bash
sudo ./engine logs alpha
sudo ./engine logs beta
```

Prints `hello from alpha` and `hello from beta` — output captured through the pipe → bounded buffer → log file pipeline.

---

## Task 4 — Kernel Memory Monitor

### Implementation

- Character device at `/dev/container_monitor`
- Supervisor registers container PIDs via `ioctl(MONITOR_REGISTER)` after each `clone()`
- Kernel linked list (`monitored_list`) stores one entry per container: PID, container ID, soft limit, hard limit, warned flag
- List protected by a **spinlock** (not mutex) because the timer callback runs in softirq context where sleeping is forbidden
- Periodic kernel timer fires every second, iterates the list:
  - If process has exited (`get_rss_bytes` returns -1) → remove stale entry
  - If RSS ≥ hard limit → send `SIGKILL`, remove entry, log to dmesg
  - If RSS ≥ soft limit and not yet warned → log warning to dmesg, set warned flag
- `list_for_each_entry_safe()` used throughout so nodes can be deleted during iteration
- All entries freed safely on `rmmod` via `timer_shutdown_sync()` before list teardown

### Synchronisation Justification

A spinlock is used instead of a mutex because the timer callback runs in softirq (bottom-half) context where sleeping is not allowed. Mutexes can sleep; spinlocks busy-wait. The critical sections (list insert/delete, scalar field reads) are short, so busy-wait overhead is negligible. The ioctl path runs in process context and can safely acquire a spinlock. `kfree()` is always called outside the spinlock by staging deletions in a local `to_free` list first.

### Build and Load

```bash
cd boilerplate
make

# Load the module
sudo insmod monitor.ko

# Verify device exists
ls -l /dev/container_monitor

# Check kernel logs
dmesg | tail -20

# Unload the module
sudo rmmod monitor
```

### Running Memory Tests

Copy the memory workload into a container rootfs before launch:

```bash
cp boilerplate/memory_hog ./rootfs-alpha/
sudo ./engine start alpha ./rootfs-alpha "/memory_hog" --soft-mib 48 --hard-mib 64
```

Watch dmesg for soft and hard limit events:

```bash
dmesg | grep container_monitor
```

Expected soft-limit output:
```
[container_monitor] SOFT LIMIT container=alpha pid=1234 rss=50331648 limit=50331648
```

Expected hard-limit output:
```
[container_monitor] HARD LIMIT container=alpha pid=1234 rss=67108864 limit=67108864 — sent SIGKILL
```

---

## Task 5 — Scheduler Experiments

Full analysis and raw data: [`scheduler_experiments.md`](scheduler_experiments.md)

To reproduce:

```bash
cd boilerplate
make ci
bash run_experiments.sh
```

### Experiment 1: Two CPU-bound workloads at different nice values

Two `cpu_hog` processes ran concurrently for 20 seconds — one at nice=0, one at nice=19.

| Metric | nice=0 (high priority) | nice=19 (low priority) |
|--------|------------------------|------------------------|
| User CPU time | 19.85s | 19.20s |
| Elapsed time | 0:19.84 | 0:19.25 |
| CPU usage | 100% | 99% |

Both completed in nearly identical time. On a multi-core host, CFS schedules both processes on separate cores simultaneously — nice values affect CPU *share* when cores are contested, not whether a process runs at all. This demonstrates CFS's throughput goal: idle cores are never wasted.

### Experiment 2: CPU-bound vs I/O-bound concurrently

`cpu_hog` (15s) and `io_pulse` (30 iterations, 100ms sleep) ran concurrently at the same priority.

| Metric | cpu_hog (CPU-bound) | io_pulse (I/O-bound) |
|--------|---------------------|----------------------|
| User CPU time | 14.50s | 0.00s |
| Elapsed time | 0:14.50 | 0:03.65 |
| CPU usage | 99% | 0% |

`io_pulse` finished 4× faster despite competing with a 100% CPU workload. I/O-bound processes voluntarily yield the CPU on every `usleep()`, keeping their vruntime low. CFS reschedules them immediately on wakeup — demonstrating the responsiveness guarantee CFS provides to interactive and I/O-bound tasks.

### Screenshots

![Experiment 1 - nice=0 vs nice=19](screenshots/exp1_nice_comparison.png)
*Two cpu_hog processes running concurrently at different priorities.*

![Experiment 2 - CPU-bound vs I/O-bound](screenshots/exp2_cpu_vs_io.png)
*cpu_hog (left, 99% CPU) vs io_pulse (right, 0% CPU, finishes 4× faster).*

---

## Engineering Analysis

### 1. Isolation Mechanisms

Each container is created with `clone()` using three namespace flags. `CLONE_NEWPID` gives the container its own PID namespace — PID 1 inside the container is a different process on the host. `CLONE_NEWUTS` gives it an independent hostname. `CLONE_NEWNS` isolates the mount namespace so mounts inside don't affect the host. `chroot()` then restricts the container's filesystem view to its assigned rootfs directory.

The host kernel is still shared across all containers — they share the same kernel, the same scheduler, and the same network stack (no `CLONE_NEWNET` in this implementation). Kernel memory, system calls, and interrupt handling are not isolated.

### 2. Supervisor and Process Lifecycle

A long-running supervisor is necessary because containers are child processes — when they exit, their parent must call `waitpid()` to collect the exit status and free kernel resources. Without a persistent parent, exited children become zombies. The supervisor installs a `SIGCHLD` handler that calls `waitpid(-1, WNOHANG)` in a loop to reap all exited children promptly. Per-container metadata is maintained in a linked list so the supervisor can track state, log paths, and exit codes across the full container lifecycle from `starting` through `running` to `stopped`, `exited`, or `hard_limit_killed`.

### 3. IPC, Threads, and Synchronisation

The project uses two IPC mechanisms. Path A uses pipes — each container's stdout/stderr file descriptors are redirected to a pipe read by a producer thread in the supervisor. Path B uses a UNIX domain socket — the CLI sends a `control_request_t` struct and reads back a `control_response_t`.

The bounded buffer shared between producer and consumer threads is protected by a mutex and two condition variables (`not_full`, `not_empty`). Without the mutex, a producer could overwrite a slot mid-read (data corruption). Without condition variables, threads would busy-wait (wasted CPU). The container metadata linked list is protected by a separate mutex to keep logging and control-plane access independent. The kernel module's monitored list uses a spinlock instead of a mutex because the timer callback runs in softirq context where sleeping is forbidden.

### 4. Memory Management and Enforcement

RSS (Resident Set Size) measures the physical RAM currently mapped to a process's pages. It does not include swapped-out pages, shared library pages counted once across all users, or memory-mapped files not yet faulted in. Soft and hard limits serve different purposes: a soft limit is a warning threshold that lets the supervisor or operator react gracefully before the situation becomes critical; a hard limit is an enforcement boundary where the kernel module sends `SIGKILL` to stop the process immediately. Enforcement belongs in kernel space because a user-space monitor can be delayed by the scheduler — a process that allocates memory rapidly could blow past its limit before the user-space poller gets CPU time. The kernel timer runs at a guaranteed rate independent of user-space scheduling.

### 5. Scheduling Behavior

Experiment 1 shows that on a multi-core system, CFS prioritizes utilization over priority enforcement — both processes ran at nearly 100% CPU simultaneously because spare cores were available. The nice=19 process was not starved. Experiment 2 shows CFS's responsiveness guarantee: `io_pulse` voluntarily yielded the CPU on every `usleep()`, keeping its vruntime far below the running average. When its sleep timer fired, CFS scheduled it immediately ahead of the CPU-bound process because it had the smallest vruntime. This is why `io_pulse` completed in 3.65s despite `cpu_hog` holding 99% CPU — the scheduler never delayed its wakeups.

---

## Design Decisions and Tradeoffs

### Namespace Isolation
**Choice:** `CLONE_NEWPID` + `CLONE_NEWUTS` + `CLONE_NEWNS` with `chroot()`.
**Tradeoff:** No network isolation (`CLONE_NEWNET` omitted) — containers share the host network stack, which simplifies implementation but means containers could interfere with each other's network connections.
**Justification:** The project scope focuses on process and filesystem isolation. Network isolation would require setting up virtual ethernet pairs and a bridge, which is out of scope.

### Supervisor Architecture
**Choice:** Single supervisor process managing all containers via a linked list.
**Tradeoff:** The linked list is O(n) for lookup — with hundreds of containers this becomes slow. A hash map would be O(1) but adds complexity.
**Justification:** For the expected container count (< 10 in any demo), O(n) lookup is negligible and a linked list is simpler to implement correctly with mutex protection.

### IPC / Logging
**Choice:** Mutex + condition variables for the bounded buffer.
**Tradeoff:** Condition variables add complexity vs a simple semaphore, but allow threads to wait on specific conditions (not_full vs not_empty) independently.
**Justification:** Two separate conditions are needed — producers wait on `not_full`, consumers wait on `not_empty`. A single semaphore can't express both independently.

### Kernel Monitor
**Choice:** Spinlock over mutex for the kernel linked list.
**Tradeoff:** Spinlocks busy-wait, wasting CPU cycles if the lock is held for long. A mutex would be more efficient if sleeping were allowed.
**Justification:** The timer callback runs in softirq context where sleeping is forbidden. Spinlock is the only correct choice on that code path. Critical sections are short so busy-wait overhead is negligible.

### Scheduling Experiments
**Choice:** `nice` values and workload type (CPU-bound vs I/O-bound) as the experimental variables.
**Tradeoff:** CPU affinity (`taskset`) would give more dramatic results on a multi-core system by forcing contention on a single core, but `nice` values directly exercise CFS priority weights as designed.
**Justification:** `nice` is the standard POSIX mechanism for scheduling priority and directly maps to CFS vruntime weight calculations, making it the most relevant variable for illustrating scheduler theory.

---

## Scheduler Experiment Results

Full data, methodology, and analysis: [`scheduler_experiments.md`](scheduler_experiments.md)

### Raw Data

**Experiment 1 — nice=0 vs nice=19 (both cpu_hog, 20 seconds, concurrent):**

```
nice=0:  19.85user 0.00system 0:19.84elapsed 100%CPU
nice=19: 19.20user 0.00system 0:19.25elapsed  99%CPU
```

**Experiment 2 — cpu_hog vs io_pulse (concurrent, same priority):**

```
cpu_hog:  14.50user 0.00system 0:14.50elapsed 99%CPU  0outputs
io_pulse:  0.00user 0.00system 0:03.65elapsed  0%CPU 240outputs
```

### What the Results Show

Experiment 1 demonstrates that CFS priority enforcement is relative — on a host with multiple available cores, both processes run freely and the nice value difference (~0.65s in user time) is small. Priority matters most when cores are scarce and processes must compete for the same core.

Experiment 2 demonstrates CFS's I/O-bound favoritism. `io_pulse` spent most of its time sleeping in `usleep()`, consuming zero CPU, while finishing 4× faster than the CPU-bound workload. This is the direct result of CFS's vruntime accounting: sleeping processes accumulate no vruntime, so they are always next in line when they wake up.