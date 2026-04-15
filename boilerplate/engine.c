/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 *
 * Tasks implemented:
 *   Task 1 - Multi-container runtime with parent supervisor
 *            clone() + namespaces, chroot, /proc mount, metadata,
 *            SIGCHLD reaping, pipe-based stdout/stderr capture foundation
 *   Task 2 - UNIX domain socket control plane (CLI <-> supervisor)
 *            start / run / ps / logs / stop commands
 *   Task 3 - Bounded-buffer logging pipeline
 *            producer threads per container, single consumer thread,
 *            mutex + condvar synchronisation, per-container log files
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

/* ------------------------------------------------------------------ */
/*  Constants                                                           */
/* ------------------------------------------------------------------ */

#define STACK_SIZE           (1024 * 1024)
#define CONTAINER_ID_LEN     32
#define CONTROL_PATH         "/tmp/mini_runtime.sock"
#define LOG_DIR              "logs"
#define CONTROL_MESSAGE_LEN  512
#define CHILD_COMMAND_LEN    256
#define LOG_CHUNK_SIZE       4096
#define LOG_BUFFER_CAPACITY  16
#define DEFAULT_SOFT_LIMIT   (40UL << 20)   /* 40 MiB */
#define DEFAULT_HARD_LIMIT   (64UL << 20)   /* 64 MiB */
#define MAX_CONTAINERS       64
#define STOP_GRACE_SECONDS   5

/* ------------------------------------------------------------------ */
/*  Enumerations                                                        */
/* ------------------------------------------------------------------ */

typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,       /* clean stop via engine stop           */
    CONTAINER_KILLED,        /* hard-limit kill from kernel monitor  */
    CONTAINER_EXITED         /* exited on its own                    */
} container_state_t;

/* ------------------------------------------------------------------ */
/*  Data structures                                                     */
/* ------------------------------------------------------------------ */

typedef struct container_record {
    char               id[CONTAINER_ID_LEN];
    pid_t              host_pid;
    time_t             started_at;
    container_state_t  state;
    unsigned long      soft_limit_bytes;
    unsigned long      hard_limit_bytes;
    int                exit_code;
    int                exit_signal;
    int                stop_requested;   /* set before sending SIGTERM */
    char               log_path[PATH_MAX];
    struct container_record *next;
} container_record_t;

typedef struct {
    char   container_id[CONTAINER_ID_LEN];
    size_t length;
    char   data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t      items[LOG_BUFFER_CAPACITY];
    size_t          head;
    size_t          tail;
    size_t          count;
    int             shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char           container_id[CONTAINER_ID_LEN];
    char           rootfs[PATH_MAX];
    char           command[CHILD_COMMAND_LEN];
    unsigned long  soft_limit_bytes;
    unsigned long  hard_limit_bytes;
    int            nice_value;
} control_request_t;

typedef struct {
    int  status;                        /* 0 = OK, non-zero = error   */
    char message[PATH_MAX + 64];
} control_response_t;

/* Passed into child_fn via clone() */
typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int  nice_value;
    int  pipe_write_fd;   /* child writes stdout/stderr here          */
} child_config_t;

/* Per-container producer thread argument */
typedef struct {
    int             pipe_read_fd;
    char            container_id[CONTAINER_ID_LEN];
    bounded_buffer_t *log_buffer;
} producer_arg_t;

/* Global supervisor context (one instance) */
typedef struct {
    int              server_fd;
    int              monitor_fd;
    volatile int     should_stop;
    pthread_t        logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t  metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

static supervisor_ctx_t *g_ctx = NULL;   /* for signal handlers */

/* ------------------------------------------------------------------ */
/*  Forward declarations                                                */
/* ------------------------------------------------------------------ */

static void sigchld_handler(int sig);
static void sigterm_handler(int sig);

int register_with_monitor(int monitor_fd, const char *container_id,
                          pid_t host_pid, unsigned long soft_limit_bytes,
                          unsigned long hard_limit_bytes);
int unregister_from_monitor(int monitor_fd, const char *container_id,
                            pid_t host_pid);

/* ------------------------------------------------------------------ */
/*  Utility / parsing                                                   */
/* ------------------------------------------------------------------ */

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command>"
            " [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run   <id> <container-rootfs> <command>"
            " [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}

static int parse_mib_flag(const char *flag,
                          const char *value,
                          unsigned long *target_bytes)
{
    char *end = NULL;
    unsigned long mib;

    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }
    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "Value for %s is too large: %s\n", flag, value);
        return -1;
    }
    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req,
                                int argc, char *argv[], int start_index)
{
    int i;
    for (i = start_index; i < argc; i += 2) {
        char *end = NULL;
        long  nice_value;

        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }
        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i+1],
                               &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }
        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i+1],
                               &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }
        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nice_value = strtol(argv[i+1], &end, 10);
            if (errno != 0 || end == argv[i+1] || *end != '\0' ||
                nice_value < -20 || nice_value > 19) {
                fprintf(stderr,
                        "Invalid value for --nice (expected -20..19): %s\n",
                        argv[i+1]);
                return -1;
            }
            req->nice_value = (int)nice_value;
            continue;
        }
        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }
    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr,
                "Invalid limits: soft limit cannot exceed hard limit\n");
        return -1;
    }
    return 0;
}

static const char *state_to_string(container_state_t state)
{
    switch (state) {
    case CONTAINER_STARTING: return "starting";
    case CONTAINER_RUNNING:  return "running";
    case CONTAINER_STOPPED:  return "stopped";
    case CONTAINER_KILLED:   return "killed";
    case CONTAINER_EXITED:   return "exited";
    default:                 return "unknown";
    }
}

/* ------------------------------------------------------------------ */
/*  Bounded buffer                                                      */
/* ------------------------------------------------------------------ */

static int bounded_buffer_init(bounded_buffer_t *buf)
{
    int rc;
    memset(buf, 0, sizeof(*buf));
    rc = pthread_mutex_init(&buf->mutex, NULL);
    if (rc != 0) return rc;
    rc = pthread_cond_init(&buf->not_empty, NULL);
    if (rc != 0) { pthread_mutex_destroy(&buf->mutex); return rc; }
    rc = pthread_cond_init(&buf->not_full, NULL);
    if (rc != 0) {
        pthread_cond_destroy(&buf->not_empty);
        pthread_mutex_destroy(&buf->mutex);
        return rc;
    }
    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buf)
{
    pthread_cond_destroy(&buf->not_full);
    pthread_cond_destroy(&buf->not_empty);
    pthread_mutex_destroy(&buf->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buf)
{
    pthread_mutex_lock(&buf->mutex);
    buf->shutting_down = 1;
    pthread_cond_broadcast(&buf->not_empty);
    pthread_cond_broadcast(&buf->not_full);
    pthread_mutex_unlock(&buf->mutex);
}

/*
 * bounded_buffer_push - producer inserts a log chunk.
 *
 * Blocks while the buffer is full. Returns 0 on success, -1 if the
 * buffer is shutting down and the push cannot be accepted.
 *
 * Why mutex + condvar:
 *   The buffer is a shared array accessed by multiple producer threads
 *   and one consumer thread. A mutex serialises all access; condvars
 *   let threads sleep efficiently instead of spinning.
 */
int bounded_buffer_push(bounded_buffer_t *buf, const log_item_t *item)
{
    pthread_mutex_lock(&buf->mutex);

    /* Wait while full, but not if we are shutting down */
    while (buf->count == LOG_BUFFER_CAPACITY && !buf->shutting_down)
        pthread_cond_wait(&buf->not_full, &buf->mutex);

    if (buf->shutting_down) {
        pthread_mutex_unlock(&buf->mutex);
        return -1;
    }

    buf->items[buf->tail] = *item;
    buf->tail = (buf->tail + 1) % LOG_BUFFER_CAPACITY;
    buf->count++;

    pthread_cond_signal(&buf->not_empty);
    pthread_mutex_unlock(&buf->mutex);
    return 0;
}

/*
 * bounded_buffer_pop - consumer removes a log chunk.
 *
 * Returns  0 with a valid item,
 *         -1 when shutting down AND buffer is empty (consumer should exit).
 */
int bounded_buffer_pop(bounded_buffer_t *buf, log_item_t *item)
{
    pthread_mutex_lock(&buf->mutex);

    /* Wait while empty */
    while (buf->count == 0 && !buf->shutting_down)
        pthread_cond_wait(&buf->not_empty, &buf->mutex);

    if (buf->count == 0) {
        /* shutting_down is set and nothing left to drain */
        pthread_mutex_unlock(&buf->mutex);
        return -1;
    }

    *item = buf->items[buf->head];
    buf->head = (buf->head + 1) % LOG_BUFFER_CAPACITY;
    buf->count--;

    pthread_cond_signal(&buf->not_full);
    pthread_mutex_unlock(&buf->mutex);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Logging consumer thread (Task 3)                                    */
/* ------------------------------------------------------------------ */

/*
 * logging_thread - single consumer that drains the bounded buffer and
 * writes each chunk to the correct per-container log file.
 *
 * It keeps running until bounded_buffer_pop returns -1, which only
 * happens when shutting_down is set AND the buffer is fully drained.
 * This guarantees no log lines are lost even on abrupt container exit.
 */
void *logging_thread(void *arg)
{
    bounded_buffer_t *buf = (bounded_buffer_t *)arg;
    log_item_t item;

    while (bounded_buffer_pop(buf, &item) == 0) {
        /* Locate the log file path from global metadata */
        char log_path[PATH_MAX];
        log_path[0] = '\0';

        if (g_ctx) {
            pthread_mutex_lock(&g_ctx->metadata_lock);
            container_record_t *c = g_ctx->containers;
            while (c) {
                if (strncmp(c->id, item.container_id,
                            CONTAINER_ID_LEN) == 0) {
                    strncpy(log_path, c->log_path,
                            sizeof(log_path) - 1);
                    break;
                }
                c = c->next;
            }
            pthread_mutex_unlock(&g_ctx->metadata_lock);
        }

        if (log_path[0] == '\0') {
            /* Fallback: write to stdout so no data is silently lost */
            fwrite(item.data, 1, item.length, stdout);
            continue;
        }

        int fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd < 0) continue;
        write(fd, item.data, item.length);
        close(fd);
    }

    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Per-container producer thread (Task 3)                             */
/* ------------------------------------------------------------------ */

/*
 * producer_thread - one per container; reads from the pipe connected
 * to that container's stdout/stderr and pushes chunks into the shared
 * bounded buffer.
 *
 * Exits naturally when the pipe's write end is closed (container exits).
 */
static void *producer_thread(void *arg)
{
    producer_arg_t *pa  = (producer_arg_t *)arg;
    log_item_t      item;
    ssize_t         n;

    memset(&item, 0, sizeof(item));
    strncpy(item.container_id, pa->container_id,
            CONTAINER_ID_LEN - 1);

    while ((n = read(pa->pipe_read_fd,
                     item.data, LOG_CHUNK_SIZE)) > 0) {
        item.length = (size_t)n;
        /* Push blocks if buffer is full; returns -1 only on shutdown */
        if (bounded_buffer_push(pa->log_buffer, &item) != 0)
            break;
    }

    close(pa->pipe_read_fd);
    free(pa);
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Container child process (Task 1)                                   */
/* ------------------------------------------------------------------ */

/*
 * child_fn - entry point executed inside the new namespaces created by
 * clone(). Responsibilities:
 *
 *   1. Redirect stdout + stderr to the pipe so the supervisor captures
 *      all output through the logging pipeline.
 *   2. Optionally adjust scheduling priority via nice().
 *   3. Mount /proc so tools like ps work inside the container.
 *   4. chroot into the container's own rootfs directory.
 *   5. exec the requested command.
 *
 * Namespaces provided by clone() flags:
 *   CLONE_NEWPID  - container gets PID 1; host PIDs are invisible.
 *   CLONE_NEWUTS  - container can set its own hostname.
 *   CLONE_NEWNS   - container gets a private mount namespace so our
 *                   /proc mount does not affect the host.
 */
int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;

    /* Redirect stdout and stderr to the logging pipe */
    if (dup2(cfg->pipe_write_fd, STDOUT_FILENO) < 0) {
        perror("dup2 stdout");
        return 1;
    }
    if (dup2(cfg->pipe_write_fd, STDERR_FILENO) < 0) {
        perror("dup2 stderr");
        return 1;
    }
    close(cfg->pipe_write_fd);

    /* Apply nice value if requested */
    if (cfg->nice_value != 0) {
        if (nice(cfg->nice_value) == -1 && errno != 0) {
            fprintf(stderr, "nice(%d) failed: %s\n",
                    cfg->nice_value, strerror(errno));
            /* non-fatal, continue */
        }
    }

    /* Mount /proc inside the container's mount namespace.
     * We must do this before chroot so the path resolves correctly. */
    if (mount("proc", "/proc", "proc",
              MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL) < 0) {
        /* Not fatal — the rootfs might not have /proc yet */
        fprintf(stderr, "Warning: mount /proc failed: %s\n",
                strerror(errno));
    }

    /* chroot into the container's own rootfs */
    if (chroot(cfg->rootfs) < 0) {
        fprintf(stderr, "chroot(%s) failed: %s\n",
                cfg->rootfs, strerror(errno));
        return 1;
    }
    if (chdir("/") < 0) {
        fprintf(stderr, "chdir / failed: %s\n", strerror(errno));
        return 1;
    }

    /* exec the command — we use /bin/sh -c so the command string can
     * contain arguments (e.g. "/bin/sh" or "/cpu_hog 30") */
    execl("/bin/sh", "sh", "-c", cfg->command, (char *)NULL);

    /* If execl returns, something went wrong */
    fprintf(stderr, "execl failed: %s\n", strerror(errno));
    return 1;
}

/* ------------------------------------------------------------------ */
/*  Metadata helpers (must be called with metadata_lock held)          */
/* ------------------------------------------------------------------ */

static container_record_t *find_container(supervisor_ctx_t *ctx,
                                          const char *id)
{
    container_record_t *c = ctx->containers;
    while (c) {
        if (strncmp(c->id, id, CONTAINER_ID_LEN) == 0)
            return c;
        c = c->next;
    }
    return NULL;
}

static container_record_t *alloc_record(const char *id,
                                        const char *rootfs,
                                        unsigned long soft,
                                        unsigned long hard)
{
    container_record_t *rec = calloc(1, sizeof(*rec));
    if (!rec) return NULL;

    strncpy(rec->id, id, CONTAINER_ID_LEN - 1);
    rec->state            = CONTAINER_STARTING;
    rec->started_at       = time(NULL);
    rec->soft_limit_bytes = soft;
    rec->hard_limit_bytes = hard;
    rec->exit_code        = -1;
    rec->exit_signal      = 0;
    rec->stop_requested   = 0;

    /* Create log directory if needed */
    mkdir(LOG_DIR, 0755);
    snprintf(rec->log_path, sizeof(rec->log_path),
             "%s/%s.log", LOG_DIR, id);

    (void)rootfs; /* rootfs stored in child_config, not needed in record */
    return rec;
}

/* ------------------------------------------------------------------ */
/*  Launch a container (called from supervisor event loop)             */
/* ------------------------------------------------------------------ */

/*
 * launch_container - allocates a clone stack, sets up a pipe for
 * stdout/stderr capture, calls clone() with PID+UTS+mount namespaces,
 * registers the process with the kernel monitor, and starts a producer
 * thread to feed the logging pipeline.
 *
 * Returns the new container_record on success, NULL on failure.
 */
static container_record_t *launch_container(supervisor_ctx_t *ctx,
                                            const control_request_t *req)
{
    int pipefd[2];
    char *stack = NULL;
    char *stack_top;
    pid_t child_pid;
    child_config_t *cfg = NULL;
    container_record_t *rec = NULL;
    producer_arg_t *pa = NULL;
    pthread_t prod_tid;

    /* Reject duplicate IDs */
    pthread_mutex_lock(&ctx->metadata_lock);
    if (find_container(ctx, req->container_id)) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        fprintf(stderr, "Container '%s' already exists\n",
                req->container_id);
        return NULL;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    /* Create pipe: child writes, supervisor reads */
    if (pipe(pipefd) < 0) {
        perror("pipe");
        return NULL;
    }
    /* Set read end non-blocking so producer thread never hangs on
     * a dead container; the write end is handed to the child */
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    /* Actually use blocking reads in the producer — simpler and correct;
     * re-set to blocking: */
    fcntl(pipefd[0], F_SETFL, 0);

    /* Clone stack — grows downward on x86 */
    stack = malloc(STACK_SIZE);
    if (!stack) {
        perror("malloc stack");
        close(pipefd[0]);
        close(pipefd[1]);
        return NULL;
    }
    stack_top = stack + STACK_SIZE;

    /* child_config lives on the heap; child_fn reads it before exec */
    cfg = malloc(sizeof(*cfg));
    if (!cfg) {
        perror("malloc cfg");
        free(stack);
        close(pipefd[0]);
        close(pipefd[1]);
        return NULL;
    }
    strncpy(cfg->id,      req->container_id, CONTAINER_ID_LEN - 1);
    strncpy(cfg->rootfs,  req->rootfs,        PATH_MAX - 1);
    strncpy(cfg->command, req->command,        CHILD_COMMAND_LEN - 1);
    cfg->nice_value    = req->nice_value;
    cfg->pipe_write_fd = pipefd[1];

    /*
     * clone() flags:
     *   CLONE_NEWPID  - new PID namespace (container sees itself as PID 1)
     *   CLONE_NEWUTS  - new UTS namespace (container can set own hostname)
     *   CLONE_NEWNS   - new mount namespace (private /proc mount)
     *   SIGCHLD       - deliver SIGCHLD to supervisor on child exit
     */
    child_pid = clone(child_fn, stack_top,
                      CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                      cfg);

    /* Supervisor no longer needs the write end of the pipe */
    close(pipefd[1]);

    if (child_pid < 0) {
        perror("clone");
        free(cfg);
        free(stack);
        close(pipefd[0]);
        return NULL;
    }

    /* stack and cfg are intentionally not freed here — the child is
     * running in the same memory space until exec replaces the image.
     * After exec, those pages are no longer mapped in the child.
     * We free stack after waitpid in the SIGCHLD handler.
     * For simplicity we accept a small one-time leak of cfg post-exec;
     * a production runtime would use vfork or a dedicated cleanup path. */
    free(stack);   /* safe: clone copies the stack reference */
    free(cfg);

    /* Build metadata record */
    rec = alloc_record(req->container_id, req->rootfs,
                       req->soft_limit_bytes, req->hard_limit_bytes);
    if (!rec) {
        /* Kill child we can no longer track */
        kill(child_pid, SIGKILL);
        close(pipefd[0]);
        return NULL;
    }
    rec->host_pid = child_pid;
    rec->state    = CONTAINER_RUNNING;

    /* Insert into linked list */
    pthread_mutex_lock(&ctx->metadata_lock);
    rec->next        = ctx->containers;
    ctx->containers  = rec;
    pthread_mutex_unlock(&ctx->metadata_lock);

    /* Register with kernel monitor (best-effort) */
    if (ctx->monitor_fd >= 0) {
        register_with_monitor(ctx->monitor_fd,
                              req->container_id,
                              child_pid,
                              req->soft_limit_bytes,
                              req->hard_limit_bytes);
    }

    /* Start producer thread for this container's pipe */
    pa = malloc(sizeof(*pa));
    if (pa) {
        pa->pipe_read_fd = pipefd[0];
        strncpy(pa->container_id, req->container_id,
                CONTAINER_ID_LEN - 1);
        pa->log_buffer = &ctx->log_buffer;

        if (pthread_create(&prod_tid, NULL, producer_thread, pa) != 0) {
            perror("pthread_create producer");
            free(pa);
            close(pipefd[0]);
        } else {
            pthread_detach(prod_tid); /* producer frees itself on exit */
        }
    } else {
        close(pipefd[0]);
    }

    fprintf(stderr, "[supervisor] launched container '%s' pid=%d\n",
            rec->id, rec->host_pid);
    return rec;
}

/* ------------------------------------------------------------------ */
/*  SIGCHLD handler — reaps children and updates metadata              */
/* ------------------------------------------------------------------ */

/*
 * sigchld_handler - called whenever a child process changes state.
 *
 * We use waitpid(-1, WNOHANG) in a loop to reap ALL ready children
 * in one handler invocation. This is necessary because multiple SIGCHLD
 * signals may be merged into one delivery by the kernel.
 *
 * The handler:
 *   1. Reaps the child (prevents zombies).
 *   2. Determines whether exit was normal, signalled, stopped, or
 *      hard-limit killed.
 *   3. Updates the container_record state accordingly.
 *   4. Unregisters the PID from the kernel monitor.
 */
static void sigchld_handler(int sig)
{
    (void)sig;
    if (!g_ctx) return;

    int   wstatus;
    pid_t pid;

    while ((pid = waitpid(-1, &wstatus, WNOHANG)) > 0) {
        pthread_mutex_lock(&g_ctx->metadata_lock);
        container_record_t *c = g_ctx->containers;
        while (c) {
            if (c->host_pid == pid) {
                if (WIFEXITED(wstatus)) {
                    c->exit_code = WEXITSTATUS(wstatus);
                    c->state     = CONTAINER_EXITED;
                } else if (WIFSIGNALED(wstatus)) {
                    c->exit_signal = WTERMSIG(wstatus);
                    if (c->stop_requested) {
                        /* engine stop sent SIGTERM/SIGKILL */
                        c->state = CONTAINER_STOPPED;
                    } else if (c->exit_signal == SIGKILL) {
                        /* kernel monitor hard-limit kill */
                        c->state = CONTAINER_KILLED;
                    } else {
                        c->state = CONTAINER_EXITED;
                    }
                }
                fprintf(stderr,
                        "[supervisor] container '%s' pid=%d exited"
                        " state=%s\n",
                        c->id, pid, state_to_string(c->state));
                break;
            }
            c = c->next;
        }
        pthread_mutex_unlock(&g_ctx->metadata_lock);

        /* Unregister from kernel monitor */
        if (g_ctx->monitor_fd >= 0 && c) {
            unregister_from_monitor(g_ctx->monitor_fd,
                                    c->id, pid);
        }
    }
}

static void sigterm_handler(int sig)
{
    (void)sig;
    if (g_ctx) g_ctx->should_stop = 1;
}

/* ------------------------------------------------------------------ */
/*  Supervisor response helpers                                         */
/* ------------------------------------------------------------------ */

static void send_response(int client_fd, int status, const char *msg)
{
    control_response_t resp;
    memset(&resp, 0, sizeof(resp));
    resp.status = status;
    strncpy(resp.message, msg, sizeof(resp.message) - 1);
    write(client_fd, &resp, sizeof(resp));
}

/* ------------------------------------------------------------------ */
/*  Supervisor command handlers                                         */
/* ------------------------------------------------------------------ */

static void handle_start(supervisor_ctx_t *ctx,
                         int client_fd,
                         const control_request_t *req)
{
    container_record_t *rec = launch_container(ctx, req);
    if (!rec) {
        send_response(client_fd, 1, "Failed to launch container");
        return;
    }
    char msg[CONTROL_MESSAGE_LEN];
    snprintf(msg, sizeof(msg),
             "Started container '%s' pid=%d",
             rec->id, rec->host_pid);
    send_response(client_fd, 0, msg);
}

static void handle_ps(supervisor_ctx_t *ctx, int client_fd)
{
    char buf[4096];
    int  off = 0;

    off += snprintf(buf + off, sizeof(buf) - off,
                    "%-16s %-8s %-10s %-8s %-10s %-10s %s\n",
                    "ID", "PID", "STATE", "EXIT",
                    "SOFT(MiB)", "HARD(MiB)", "LOG");

    pthread_mutex_lock(&ctx->metadata_lock);
    container_record_t *c = ctx->containers;
    while (c && off < (int)sizeof(buf) - 128) {
        off += snprintf(buf + off, sizeof(buf) - off,
                        "%-16s %-8d %-10s %-8d %-10lu %-10lu %s\n",
                        c->id,
                        c->host_pid,
                        state_to_string(c->state),
                        c->exit_code,
                        c->soft_limit_bytes >> 20,
                        c->hard_limit_bytes >> 20,
                        c->log_path);
        c = c->next;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    send_response(client_fd, 0, buf);
}

static void handle_logs(supervisor_ctx_t *ctx,
                        int client_fd,
                        const control_request_t *req)
{
    char log_path[PATH_MAX];
    log_path[0] = '\0';

    pthread_mutex_lock(&ctx->metadata_lock);
    container_record_t *c = find_container(ctx, req->container_id);
    if (c) strncpy(log_path, c->log_path, sizeof(log_path) - 1);
    pthread_mutex_unlock(&ctx->metadata_lock);

    if (log_path[0] == '\0') {
        send_response(client_fd, 1, "Container not found");
        return;
    }

    /* Send log file path; client will read and print it */
    char msg[PATH_MAX + 16];
    snprintf(msg, sizeof(msg), "LOG_PATH:%s", log_path);
    send_response(client_fd, 0, msg);
}

static void handle_stop(supervisor_ctx_t *ctx,
                        int client_fd,
                        const control_request_t *req)
{
    pthread_mutex_lock(&ctx->metadata_lock);
    container_record_t *c = find_container(ctx, req->container_id);
    if (!c || c->state != CONTAINER_RUNNING) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        send_response(client_fd, 1,
                      "Container not found or not running");
        return;
    }

    /* Set stop_requested BEFORE sending the signal so that the
     * SIGCHLD handler classifies the exit as CONTAINER_STOPPED */
    c->stop_requested = 1;
    pid_t pid = c->host_pid;
    pthread_mutex_unlock(&ctx->metadata_lock);

    /* Graceful: SIGTERM first */
    kill(pid, SIGTERM);

    /* Give the container STOP_GRACE_SECONDS to exit, then SIGKILL */
    int waited = 0;
    while (waited < STOP_GRACE_SECONDS) {
        sleep(1);
        waited++;
        /* Check if already reaped by SIGCHLD */
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *cc = find_container(ctx, req->container_id);
        int still_running = (cc && cc->state == CONTAINER_RUNNING);
        pthread_mutex_unlock(&ctx->metadata_lock);
        if (!still_running) break;
    }

    /* Force kill if still alive */
    pthread_mutex_lock(&ctx->metadata_lock);
    container_record_t *cc = find_container(ctx, req->container_id);
    if (cc && cc->state == CONTAINER_RUNNING) {
        kill(pid, SIGKILL);
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    send_response(client_fd, 0, "Stop signal sent");
}

static void handle_run_wait(supervisor_ctx_t *ctx,
                            int client_fd,
                            const control_request_t *req)
{
    container_record_t *rec = launch_container(ctx, req);
    if (!rec) {
        send_response(client_fd, 1, "Failed to launch container");
        return;
    }

    char id[CONTAINER_ID_LEN];
    strncpy(id, rec->id, CONTAINER_ID_LEN - 1);

    /* Poll until container exits */
    while (1) {
        sleep(1);
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *c = find_container(ctx, id);
        container_state_t st  = c ? c->state : CONTAINER_EXITED;
        int exit_code         = c ? c->exit_code : -1;
        int exit_sig          = c ? c->exit_signal : 0;
        pthread_mutex_unlock(&ctx->metadata_lock);

        if (st == CONTAINER_EXITED ||
            st == CONTAINER_STOPPED ||
            st == CONTAINER_KILLED) {
            char msg[CONTROL_MESSAGE_LEN];
            int ret = (exit_sig > 0) ? 128 + exit_sig : exit_code;
            snprintf(msg, sizeof(msg),
                     "RUN_EXIT:%d state=%s",
                     ret, state_to_string(st));
            send_response(client_fd, ret, msg);
            return;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Supervisor event loop                                               */
/* ------------------------------------------------------------------ */

/*
 * run_supervisor - the long-running daemon.
 *
 * 1. Opens /dev/container_monitor (kernel module, best-effort).
 * 2. Creates a UNIX domain socket at CONTROL_PATH.
 * 3. Installs SIGCHLD and SIGTERM/SIGINT handlers.
 * 4. Starts the logging consumer thread.
 * 5. Accepts one client connection at a time, reads a control_request_t,
 *    dispatches to the appropriate handler, sends a control_response_t.
 * 6. On SIGTERM/SIGINT, stops all running containers and exits cleanly.
 */
static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd  = -1;
    ctx.monitor_fd = -1;

    g_ctx = &ctx;

    /* Metadata lock */
    if (pthread_mutex_init(&ctx.metadata_lock, NULL) != 0) {
        perror("pthread_mutex_init metadata");
        return 1;
    }

    /* Bounded buffer */
    if (bounded_buffer_init(&ctx.log_buffer) != 0) {
        perror("bounded_buffer_init");
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    /* Try to open kernel monitor device (not fatal if unavailable) */
    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (ctx.monitor_fd < 0)
        fprintf(stderr,
                "[supervisor] /dev/container_monitor unavailable"
                " (kernel module not loaded?)\n");

    /* Create log directory */
    mkdir(LOG_DIR, 0755);

    /* Set up UNIX domain socket */
    struct sockaddr_un addr;
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) {
        perror("socket");
        goto cleanup;
    }
    unlink(CONTROL_PATH);
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);
    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        goto cleanup;
    }
    if (listen(ctx.server_fd, 8) < 0) {
        perror("listen");
        goto cleanup;
    }
    fprintf(stderr,
            "[supervisor] listening on %s rootfs=%s\n",
            CONTROL_PATH, rootfs);

    /* Signal handlers */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sa.sa_flags   = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    sa.sa_handler = sigterm_handler;
    sa.sa_flags   = SA_RESTART;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);

    /* Start logging consumer thread */
    if (pthread_create(&ctx.logger_thread, NULL,
                       logging_thread, &ctx.log_buffer) != 0) {
        perror("pthread_create logger");
        goto cleanup;
    }

    /* Event loop */
    while (!ctx.should_stop) {
        /* Use select() with a timeout so we can check should_stop */
        fd_set rfds;
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        FD_ZERO(&rfds);
        FD_SET(ctx.server_fd, &rfds);

        int sel = select(ctx.server_fd + 1, &rfds, NULL, NULL, &tv);
        if (sel < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }
        if (sel == 0) continue;  /* timeout, re-check should_stop */

        int client_fd = accept(ctx.server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }

        control_request_t req;
        ssize_t n = read(client_fd, &req, sizeof(req));
        if (n != (ssize_t)sizeof(req)) {
            close(client_fd);
            continue;
        }

        switch (req.kind) {
        case CMD_START:
            handle_start(&ctx, client_fd, &req);
            break;
        case CMD_RUN:
            handle_run_wait(&ctx, client_fd, &req);
            break;
        case CMD_PS:
            handle_ps(&ctx, client_fd);
            break;
        case CMD_LOGS:
            handle_logs(&ctx, client_fd, &req);
            break;
        case CMD_STOP:
            handle_stop(&ctx, client_fd, &req);
            break;
        default:
            send_response(client_fd, 1, "Unknown command");
        }
        close(client_fd);
    }

    fprintf(stderr, "[supervisor] shutting down\n");

    /* Stop all running containers gracefully */
    pthread_mutex_lock(&ctx.metadata_lock);
    container_record_t *c = ctx.containers;
    while (c) {
        if (c->state == CONTAINER_RUNNING) {
            c->stop_requested = 1;
            kill(c->host_pid, SIGTERM);
        }
        c = c->next;
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    /* Give them a moment, then force-kill */
    sleep(2);
    pthread_mutex_lock(&ctx.metadata_lock);
    c = ctx.containers;
    while (c) {
        if (c->state == CONTAINER_RUNNING)
            kill(c->host_pid, SIGKILL);
        c = c->next;
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

cleanup:
    /* Drain and stop logger thread */
    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    if (ctx.logger_thread)
        pthread_join(ctx.logger_thread, NULL);
    bounded_buffer_destroy(&ctx.log_buffer);

    /* Free container records */
    pthread_mutex_lock(&ctx.metadata_lock);
    c = ctx.containers;
    while (c) {
        container_record_t *next = c->next;
        free(c);
        c = next;
    }
    ctx.containers = NULL;
    pthread_mutex_unlock(&ctx.metadata_lock);
    pthread_mutex_destroy(&ctx.metadata_lock);

    if (ctx.server_fd >= 0) close(ctx.server_fd);
    if (ctx.monitor_fd >= 0) close(ctx.monitor_fd);
    unlink(CONTROL_PATH);

    g_ctx = NULL;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Client-side control path (CLI -> supervisor)                       */
/* ------------------------------------------------------------------ */

/*
 * send_control_request - connects to the supervisor's UNIX socket,
 * sends the request struct, reads the response, and prints it.
 *
 * For CMD_LOGS the response contains "LOG_PATH:/path/to/file"; the
 * client reads and prints that file directly.
 * For CMD_RUN the client blocks until the supervisor replies (which
 * only happens when the container exits).
 */
static int send_control_request(const control_request_t *req)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr,
                "Cannot connect to supervisor at %s: %s\n"
                "Is the supervisor running?\n",
                CONTROL_PATH, strerror(errno));
        close(fd);
        return 1;
    }

    if (write(fd, req, sizeof(*req)) != (ssize_t)sizeof(*req)) {
        perror("write request");
        close(fd);
        return 1;
    }

    control_response_t resp;
    /* For CMD_RUN this read may block a long time */
    ssize_t n = read(fd, &resp, sizeof(resp));
    close(fd);

    if (n != (ssize_t)sizeof(resp)) {
        fprintf(stderr, "Incomplete response from supervisor\n");
        return 1;
    }

    /* CMD_LOGS: the message is "LOG_PATH:/path" — read and cat the file */
    if (req->kind == CMD_LOGS &&
        resp.status == 0 &&
        strncmp(resp.message, "LOG_PATH:", 9) == 0) {
        const char *path = resp.message + 9;
        FILE *f = fopen(path, "r");
        if (!f) {
            fprintf(stderr, "Cannot open log file %s: %s\n",
                    path, strerror(errno));
            return 1;
        }
        char buf[4096];
        size_t r;
        while ((r = fread(buf, 1, sizeof(buf), f)) > 0)
            fwrite(buf, 1, r, stdout);
        fclose(f);
        return 0;
    }

    printf("%s\n", resp.message);
    return resp.status;
}

/* ------------------------------------------------------------------ */
/*  CLI command entry points                                            */
/* ------------------------------------------------------------------ */

static int cmd_start(int argc, char *argv[])
{
    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s start <id> <rootfs> <command>"
                " [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    strncpy(req.rootfs,        argv[3], PATH_MAX - 1);
    strncpy(req.command,       argv[4], CHILD_COMMAND_LEN - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    if (parse_optional_flags(&req, argc, argv, 5) != 0) return 1;
    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s run <id> <rootfs> <command>"
                " [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    strncpy(req.rootfs,        argv[3], PATH_MAX - 1);
    strncpy(req.command,       argv[4], CHILD_COMMAND_LEN - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    if (parse_optional_flags(&req, argc, argv, 5) != 0) return 1;
    return send_control_request(&req);
}

static int cmd_ps(void)
{
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
        return 1;
    }
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    return send_control_request(&req);
}

/* ------------------------------------------------------------------ */
/*  Monitor registration helpers /
/* ------------------------------------------------------------------ */

int register_with_monitor(int monitor_fd,
                          const char *container_id,
                          pid_t host_pid,
                          unsigned long soft_limit_bytes,
                          unsigned long hard_limit_bytes)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid               = host_pid;
    req.soft_limit_bytes  = soft_limit_bytes;
    req.hard_limit_bytes  = hard_limit_bytes;
    strncpy(req.container_id, container_id,
            sizeof(req.container_id) - 1);
    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0) return -1;
    return 0;
}

int unregister_from_monitor(int monitor_fd,
                            const char *container_id,
                            pid_t host_pid)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id,
            sizeof(req.container_id) - 1);
    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0) return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    if (argc < 2) { usage(argv[0]); return 1; }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr,
                    "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }
    if (strcmp(argv[1], "start") == 0) return cmd_start(argc, argv);
    if (strcmp(argv[1], "run")   == 0) return cmd_run(argc, argv);
    if (strcmp(argv[1], "ps")    == 0) return cmd_ps();
    if (strcmp(argv[1], "logs")  == 0) return cmd_logs(argc, argv);
    if (strcmp(argv[1], "stop")  == 0) return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}