# Multi-Container Runtime

A lightweight Linux container runtime in C — a stripped-down Docker-like system with a long-running supervisor, isolated containers, a logging pipeline, and a kernel-space memory monitor.

---

## Team

| Person | Tasks |
|--------|-------|
| Shreyashree | Task 1 - Multi-Container Runtime, Task 2 - CLI & Signal Handling, Task 3 - Bounded-Buffer Logging |
| TBD | Task 4 - Kernel Memory Monitor |
| TBD | Task 5 - Scheduler Experiments | 
| TBD | Task 6 - Cleanup |

---

## Project Overview

The runtime is a single binary (`engine`) used two ways:
- **Supervisor daemon** - stays alive, manages containers, owns the logging pipeline
- **CLI client** - short-lived process that sends commands to the supervisor over a UNIX socket

Two IPC paths:
- **Path A (logging):** container stdout/stderr → pipe → producer thread → bounded buffer → consumer thread → log file
- **Path B (control):** CLI process → UNIX domain socket → supervisor → response

---

## Files (shreyashree)

| File | Purpose |
|------|---------|
| `boilerplate/engine.c` | user-space runtime: supervisor, CLI, logging pipeline |
| `boilerplate/monitor_ioctl.h` | shared ioctl definitions (used by engine.c to talk to kernel module) |
| `boilerplate/Makefile` | to build engine and kernel module |

---

## Implementstion (shreyashree)

### Task 1 - Multi-Container Runtime
- Supervisor process stays alive and manages multiple containers concurrently
- Each container launched via `clone()` with `CLONE_NEWPID`, `CLONE_NEWUTS`, `CLONE_NEWNS` — isolated PID, UTS, and mount namespaces
- `chroot()` locks each container into its own rootfs directory
- `/proc` mounted inside each container so tools like `ps` work
- Per-container metadata tracked in a mutex-protected linked list (PID, state, limits, log path, exit code)
- `SIGCHLD` handler calls `waitpid()` in a loop to reap all exited children — no zombies

### Task 2 - CLI and Signal Handling
- UNIX domain socket at `/tmp/mini_runtime.sock` connects CLI to supervisor
- CLI sends a `control_request_t` struct, reads back a `control_response_t`, prints and exits
- Commands: `start`, `run`, `ps`, `logs`, `stop`
- `stop` sends `SIGTERM` first, waits 5 seconds, then `SIGKILL`
- `stop_requested` flag set before signalling so SIGCHLD handler correctly classifies exit as `stopped` vs `killed` vs `exited`
- `SIGINT`/`SIGTERM` to supervisor triggers orderly shutdown — stops all containers, joins threads, cleans up socket

### Task 3 - Bounded-Buffer Logging
- Each container's stdout/stderr piped to a dedicated producer thread
- Producer pushes 4KB chunks into a shared bounded buffer (16 slots)
- Single consumer thread drains buffer and writes to `logs/<id>.log`
- Mutex + condition variables (`not_full`, `not_empty`) — threads sleep instead of spinning
- On shutdown, condvars are broadcast so threads wake, drain remaining data, and exit cleanly
- So no log lines lost

---

## Setup

Requires Ubuntu 22.04 or 24.04 VM, Secure Boot OFF. 
Not compatible on WSL. 

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
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

Build:

```bash
cd boilerplate
make
```

---

## Running (Tasks 1, 2, 3)

**Terminal 1 - start the supervisor (leave it running):**
```bash
sudo ./engine supervisor ./rootfs-base
```
Expected: `[supervisor] listening on /tmp/mini_runtime.sock rootfs=./rootfs-base`

**Terminal 2 - all CLI commands:**

Task 1 - launch two containers and check metadata:
```bash
sudo ./engine start alpha ./rootfs-alpha "echo hello from alpha"
sudo ./engine start beta ./rootfs-beta "echo hello from beta"
sudo ./engine ps
```
`ps` prints a table of both containers with PID, state, memory limits, log path.

Task 2 - stop a container and verify state change:
```bash
sudo ./engine start gamma ./rootfs-alpha "sleep 30"
sudo ./engine ps
sudo ./engine stop gamma
sudo ./engine ps
```
gamma shows `running` after start, `stopped` after stop.

Task 3 - verify logging pipeline:
```bash
sudo ./engine logs alpha
sudo ./engine logs beta
```
Prints `hello from alpha` and `hello from beta` — output captured through the pipe → bounded buffer → log file pipeline.

---

## CI Smoke Check

```bash
make -C boilerplate ci
```
Compiles user-space binaries only — no kernel module, no supervisor required.
