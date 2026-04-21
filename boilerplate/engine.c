/*
 * engine.c - Multi-Container Runtime Supervisor
 *
 * Implements:
 *   - Long-running supervisor process
 *   - Multi-container management with namespace isolation
 *   - CLI commands: start, run, ps, logs, stop
 *   - Bounded-buffer logging via pipes
 *   - UNIX domain socket IPC for CLI<->supervisor
 *   - SIGCHLD / SIGTERM / SIGINT handling
 *   - ioctl integration with kernel monitor
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/syscall.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <sched.h>
#include <pthread.h>
#include <stdint.h>

#include "monitor_ioctl.h"

/* ------------------------------------------------------------------ */
/*  Constants                                                           */
/* ------------------------------------------------------------------ */
#define MAX_CONTAINERS     16
#define LOG_BUF_SIZE       4096   /* bounded buffer capacity (bytes)   */
#define LOG_DIR            "/tmp/container_logs"
#define SOCKET_PATH        "/tmp/engine.sock"
#define MONITOR_DEV        "/dev/container_monitor"
#define CMD_MAX            512
#define MAX_NAME           64
#define MAX_PATH           256

/* ------------------------------------------------------------------ */
/*  Container state                                                     */
/* ------------------------------------------------------------------ */
typedef enum {
    CS_STARTING = 0,
    CS_RUNNING,
    CS_STOPPED,
    CS_KILLED,
    CS_EXITED,
} ContainerState;

static const char *state_str(ContainerState s) {
    switch (s) {
        case CS_STARTING: return "starting";
        case CS_RUNNING:  return "running";
        case CS_STOPPED:  return "stopped";
        case CS_KILLED:   return "killed";
        case CS_EXITED:   return "exited";
        default:          return "unknown";
    }
}

typedef struct {
    char            name[MAX_NAME];
    pid_t           host_pid;
    time_t          start_time;
    ContainerState  state;
    long            soft_limit_mb;  /* 0 = unset */
    long            hard_limit_mb;  /* 0 = unset */
    char            log_path[MAX_PATH];
    int             exit_status;
    int             term_signal;

    /* logging pipe: container writes to pipe_write, logger reads from pipe_read */
    int             pipe_read;
    int             pipe_write;

    /* per-container logger thread */
    pthread_t       logger_tid;
    int             logger_running;

    int             used;           /* slot in use? */
} Container;

/* ------------------------------------------------------------------ */
/*  Bounded log buffer (shared between producer(s) and consumer)       */
/* ------------------------------------------------------------------ */
typedef struct {
    char            buf[LOG_BUF_SIZE];
    int             head;
    int             tail;
    int             count;
    pthread_mutex_t lock;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
    int             done;           /* signal consumer to drain and exit */
} LogBuffer;

/* ------------------------------------------------------------------ */
/*  Logger thread argument                                              */
/* ------------------------------------------------------------------ */
typedef struct {
    Container  *c;
    LogBuffer  *lb;
} LoggerArg;

/* ------------------------------------------------------------------ */
/*  Globals                                                             */
/* ------------------------------------------------------------------ */
static Container        g_containers[MAX_CONTAINERS];
static pthread_mutex_t  g_container_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int     g_running = 1;
static int              g_monitor_fd = -1;   /* /dev/container_monitor */

/* ------------------------------------------------------------------ */
/*  Utility helpers                                                     */
/* ------------------------------------------------------------------ */
static void timestamp(char *buf, size_t n) {
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    strftime(buf, n, "%Y-%m-%dT%H:%M:%S", tm);
}

static Container *find_container(const char *name) {
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        if (g_containers[i].used &&
            strcmp(g_containers[i].name, name) == 0)
            return &g_containers[i];
    }
    return NULL;
}

static Container *alloc_container(void) {
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        if (!g_containers[i].used) {
            memset(&g_containers[i], 0, sizeof(Container));
            g_containers[i].used = 1;
            g_containers[i].pipe_read  = -1;
            g_containers[i].pipe_write = -1;
            return &g_containers[i];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Bounded-buffer: producer side (writes bytes into buffer)            */
/* ------------------------------------------------------------------ */
static void lb_produce(LogBuffer *lb, const char *data, int len) {
    pthread_mutex_lock(&lb->lock);
    for (int i = 0; i < len; i++) {
        while (lb->count == LOG_BUF_SIZE) {
            /* buffer full – wait for consumer */
            pthread_cond_wait(&lb->not_full, &lb->lock);
        }
        lb->buf[lb->tail] = data[i];
        lb->tail = (lb->tail + 1) % LOG_BUF_SIZE;
        lb->count++;
    }
    pthread_cond_signal(&lb->not_empty);
    pthread_mutex_unlock(&lb->lock);
}

/* ------------------------------------------------------------------ */
/*  Bounded-buffer: consumer side (reads one byte)                      */
/* ------------------------------------------------------------------ */
static int lb_consume(LogBuffer *lb, char *out) {
    pthread_mutex_lock(&lb->lock);
    while (lb->count == 0 && !lb->done) {
        pthread_cond_wait(&lb->not_empty, &lb->lock);
    }
    if (lb->count == 0) {
        pthread_mutex_unlock(&lb->lock);
        return 0; /* done and empty */
    }
    *out = lb->buf[lb->head];
    lb->head = (lb->head + 1) % LOG_BUF_SIZE;
    lb->count--;
    pthread_cond_signal(&lb->not_full);
    pthread_mutex_unlock(&lb->lock);
    return 1;
}

/* ------------------------------------------------------------------ */
/*  Logger thread: reads from pipe -> bounded buffer -> log file        */
/* ------------------------------------------------------------------ */
static void *logger_consumer(void *arg) {
    LoggerArg *la = (LoggerArg *)arg;
    Container *c  = la->c;
    LogBuffer *lb = la->lb;

    int logfd = open(c->log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (logfd < 0) {
        fprintf(stderr, "[engine] Cannot open log %s: %s\n",
                c->log_path, strerror(errno));
        free(la);
        return NULL;
    }

    char ch;
    while (lb_consume(lb, &ch)) {
        write(logfd, &ch, 1);
    }
    close(logfd);
    free(la);
    return NULL;
}

static void *logger_producer(void *arg) {
    LoggerArg *la = (LoggerArg *)arg;
    Container *c  = la->c;
    LogBuffer *lb = la->lb;

    /* start a consumer thread */
    LoggerArg *cons_arg = malloc(sizeof(LoggerArg));
    *cons_arg = *la;
    pthread_t cons_tid;
    pthread_create(&cons_tid, NULL, logger_consumer, cons_arg);

    char tmp[256];
    ssize_t n;
    while ((n = read(c->pipe_read, tmp, sizeof(tmp))) > 0) {
        lb_produce(lb, tmp, (int)n);
    }

    /* signal consumer that production is done */
    pthread_mutex_lock(&lb->lock);
    lb->done = 1;
    pthread_cond_signal(&lb->not_empty);
    pthread_mutex_unlock(&lb->lock);

    pthread_join(cons_tid, NULL);

    /* free the shared log buffer */
    pthread_mutex_destroy(&lb->lock);
    pthread_cond_destroy(&lb->not_empty);
    pthread_cond_destroy(&lb->not_full);
    free(lb);
    free(la);
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Namespace / container setup (runs in child after clone/fork)        */
/* ------------------------------------------------------------------ */
static void setup_container(const char *rootfs, const char *argv0,
                             char *const argv[], int pipe_write) {
    /* Redirect stdout and stderr to the logging pipe */
    dup2(pipe_write, STDOUT_FILENO);
    dup2(pipe_write, STDERR_FILENO);
    close(pipe_write);

    /* Mount proc inside container */
    char proc_path[MAX_PATH];
    snprintf(proc_path, sizeof(proc_path), "%s/proc", rootfs);
    mkdir(proc_path, 0755);
    if (mount("proc", proc_path, "proc", 0, NULL) < 0) {
        /* non-fatal if already mounted */
    }

    /* chroot into rootfs */
    if (chroot(rootfs) < 0) {
        perror("chroot");
        exit(1);
    }
    if (chdir("/") < 0) {
        perror("chdir /");
        exit(1);
    }

    /* Set hostname to container name (UTS namespace was already cloned) */
    /* hostname is passed in argv[0] pre-exec by convention (we skip it) */

    /* Execute the requested command */
    execvp(argv0, argv);
    perror("execvp");
    exit(127);
}

/* ------------------------------------------------------------------ */
/*  Register PID with kernel monitor                                    */
/* ------------------------------------------------------------------ */
static void monitor_register(pid_t pid, long soft_mb, long hard_mb) {
    if (g_monitor_fd < 0) return;
    struct container_info ci;
    ci.pid       = pid;
    ci.soft_limit = (unsigned long)soft_mb * 1024 * 1024;
    ci.hard_limit = (unsigned long)hard_mb * 1024 * 1024;
    if (ioctl(g_monitor_fd, MONITOR_REGISTER, &ci) < 0) {
        fprintf(stderr, "[engine] ioctl MONITOR_REGISTER failed: %s\n",
                strerror(errno));
    }
}

static void monitor_unregister(pid_t pid) {
    if (g_monitor_fd < 0) return;
    struct container_info ci;
    ci.pid = pid;
    if (ioctl(g_monitor_fd, MONITOR_UNREGISTER, &ci) < 0) {
        /* ignore – process may already be gone */
    }
}

/* ------------------------------------------------------------------ */
/*  Launch a container                                                  */
/* ------------------------------------------------------------------ */
static int do_start(const char *name, const char *rootfs,
                    char *const cmd_argv[], int foreground,
                    long soft_mb, long hard_mb) {
    pthread_mutex_lock(&g_container_lock);

    if (find_container(name)) {
        pthread_mutex_unlock(&g_container_lock);
        fprintf(stderr, "[engine] Container '%s' already exists\n", name);
        return -1;
    }

    Container *c = alloc_container();
    if (!c) {
        pthread_mutex_unlock(&g_container_lock);
        fprintf(stderr, "[engine] Too many containers\n");
        return -1;
    }

    strncpy(c->name, name, MAX_NAME - 1);
    c->soft_limit_mb = soft_mb;
    c->hard_limit_mb = hard_mb;
    c->start_time    = time(NULL);
    c->state         = CS_STARTING;

    /* Create log dir and path */
    mkdir(LOG_DIR, 0755);
    snprintf(c->log_path, sizeof(c->log_path), "%s/%s.log", LOG_DIR, name);

    /* Create pipe for container stdout/stderr -> supervisor */
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        perror("pipe");
        c->used = 0;
        pthread_mutex_unlock(&g_container_lock);
        return -1;
    }
    c->pipe_read  = pipefd[0];
    c->pipe_write = pipefd[1];

    /* Allocate and initialise the log buffer */
    LogBuffer *lb = calloc(1, sizeof(LogBuffer));
    pthread_mutex_init(&lb->lock, NULL);
    pthread_cond_init(&lb->not_empty, NULL);
    pthread_cond_init(&lb->not_full, NULL);

    /* Start producer (logger) thread */
    LoggerArg *la = malloc(sizeof(LoggerArg));
    la->c  = c;
    la->lb = lb;
    pthread_create(&c->logger_tid, NULL, logger_producer, la);
    c->logger_running = 1;

    /* Fork the container child */
    int clone_flags = CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD;
    pid_t pid = syscall(SYS_clone, clone_flags, NULL, NULL, NULL, NULL);

    if (pid < 0) {
        /* Clone failed */
        perror("clone");
        close(pipefd[0]);
        close(pipefd[1]);
        c->used = 0;
        pthread_mutex_unlock(&g_container_lock);
        return -1;
    }

    if (pid == 0) {
        /* --- child --- */
        close(pipefd[0]); /* child doesn't need the read end */
        /* Write timestamp header to log */
        char hdr[128];
        char ts[64];
        timestamp(ts, sizeof(ts));
        int n = snprintf(hdr, sizeof(hdr),
                         "=== Container '%s' started at %s ===\n", name, ts);
        write(pipefd[1], hdr, n);

        sethostname(name, strlen(name));
        setup_container(rootfs, cmd_argv[0], cmd_argv, pipefd[1]);
        /* never reached */
        exit(1);
    }

    /* --- parent --- */
    close(pipefd[1]);   /* parent only reads */
    c->host_pid = pid;
    c->state    = CS_RUNNING;

    char ts[64];
    timestamp(ts, sizeof(ts));
    printf("[engine] Started container '%s' pid=%d at %s\n", name, pid, ts);

    /* Register with kernel monitor if limits set */
    if (soft_mb > 0 || hard_mb > 0) {
        monitor_register(pid, soft_mb, hard_mb);
    }

    pthread_mutex_unlock(&g_container_lock);

    if (foreground) {
        int wstatus;
        waitpid(pid, &wstatus, 0);
        pthread_mutex_lock(&g_container_lock);
        if (WIFEXITED(wstatus)) {
            c->exit_status = WEXITSTATUS(wstatus);
            c->state = CS_EXITED;
        } else if (WIFSIGNALED(wstatus)) {
            c->term_signal = WTERMSIG(wstatus);
            c->state = CS_KILLED;
        }
        monitor_unregister(pid);
        pthread_mutex_unlock(&g_container_lock);
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/*  Stop a container                                                    */
/* ------------------------------------------------------------------ */
static int do_stop(const char *name) {
    pthread_mutex_lock(&g_container_lock);
    Container *c = find_container(name);
    if (!c) {
        pthread_mutex_unlock(&g_container_lock);
        fprintf(stderr, "[engine] No container named '%s'\n", name);
        return -1;
    }
    if (c->state != CS_RUNNING) {
        pthread_mutex_unlock(&g_container_lock);
        fprintf(stderr, "[engine] Container '%s' is not running\n", name);
        return -1;
    }
    kill(c->host_pid, SIGTERM);
    c->state = CS_STOPPED;
    monitor_unregister(c->host_pid);
    pthread_mutex_unlock(&g_container_lock);
    printf("[engine] Sent SIGTERM to container '%s' (pid=%d)\n",
           name, c->host_pid);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  List containers                                                     */
/* ------------------------------------------------------------------ */
static void do_ps(int out_fd) {
    char line[512];
    int n = snprintf(line, sizeof(line),
        "%-16s %-8s %-10s %-10s %-10s %-8s %-8s %s\n",
        "NAME", "PID", "STATE", "SOFT(MB)", "HARD(MB)", "EXIT", "SIG", "LOG");
    write(out_fd, line, n);

    pthread_mutex_lock(&g_container_lock);
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        Container *c = &g_containers[i];
        if (!c->used) continue;
        char ts[32];
        struct tm *tm = localtime(&c->start_time);
        strftime(ts, sizeof(ts), "%H:%M:%S", tm);
        n = snprintf(line, sizeof(line),
            "%-16s %-8d %-10s %-10ld %-10ld %-8d %-8d %s\n",
            c->name, (int)c->host_pid, state_str(c->state),
            c->soft_limit_mb, c->hard_limit_mb,
            c->exit_status, c->term_signal, c->log_path);
        write(out_fd, line, n);
    }
    pthread_mutex_unlock(&g_container_lock);
}

/* ------------------------------------------------------------------ */
/*  Show logs                                                           */
/* ------------------------------------------------------------------ */
static void do_logs(const char *name, int out_fd) {
    pthread_mutex_lock(&g_container_lock);
    Container *c = find_container(name);
    if (!c) {
        pthread_mutex_unlock(&g_container_lock);
        char msg[128];
        int n = snprintf(msg, sizeof(msg), "Error: no container '%s'\n", name);
        write(out_fd, msg, n);
        return;
    }
    char path[MAX_PATH];
    strncpy(path, c->log_path, sizeof(path) - 1);
    pthread_mutex_unlock(&g_container_lock);

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        char msg[128];
        int n = snprintf(msg, sizeof(msg), "Log not found: %s\n", path);
        write(out_fd, msg, n);
        return;
    }
    char buf[4096];
    ssize_t r;
    while ((r = read(fd, buf, sizeof(buf))) > 0)
        write(out_fd, buf, r);
    close(fd);
}

/* ------------------------------------------------------------------ */
/*  SIGCHLD handler – reap children, update state                      */
/* ------------------------------------------------------------------ */
static void sigchld_handler(int sig) {
    (void)sig;
    int saved_errno = errno;
    int wstatus;
    pid_t pid;
    while ((pid = waitpid(-1, &wstatus, WNOHANG)) > 0) {
        /* find container by pid – must be async-signal-safe lookup */
        for (int i = 0; i < MAX_CONTAINERS; i++) {
            Container *c = &g_containers[i];
            if (c->used && c->host_pid == pid) {
                if (WIFEXITED(wstatus)) {
                    c->exit_status = WEXITSTATUS(wstatus);
                    c->state = CS_EXITED;
                } else if (WIFSIGNALED(wstatus)) {
                    c->term_signal = WTERMSIG(wstatus);
                    c->state = (WTERMSIG(wstatus) == SIGKILL)
                               ? CS_KILLED : CS_STOPPED;
                }
                /* close the write end to let the logger thread finish */
                if (c->pipe_write >= 0) {
                    close(c->pipe_write);
                    c->pipe_write = -1;
                }
                break;
            }
        }
    }
    errno = saved_errno;
}

static void sigterm_handler(int sig) {
    (void)sig;
    g_running = 0;
}

/* ------------------------------------------------------------------ */
/*  Supervisor: UNIX socket command loop                                */
/* ------------------------------------------------------------------ */
static void supervisor_loop(const char *rootfs) {
    /* Set up signal handlers */
    struct sigaction sa_chld = {0};
    sa_chld.sa_handler = sigchld_handler;
    sa_chld.sa_flags   = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa_chld, NULL);

    struct sigaction sa_term = {0};
    sa_term.sa_handler = sigterm_handler;
    sigaction(SIGTERM, &sa_term, NULL);
    sigaction(SIGINT,  &sa_term, NULL);

    /* Open kernel monitor if available */
    g_monitor_fd = open(MONITOR_DEV, O_RDWR);
    if (g_monitor_fd < 0)
        fprintf(stderr, "[engine] Monitor device unavailable (%s) – "
                "memory limits disabled\n", strerror(errno));

    /* UNIX domain socket */
    unlink(SOCKET_PATH);
    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); exit(1); }

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); exit(1);
    }
    listen(srv, 8);
    chmod(SOCKET_PATH, 0666);

    printf("[engine] Supervisor running. Socket: %s\n", SOCKET_PATH);
    printf("[engine] Rootfs: %s\n", rootfs);

    /* Make socket non-blocking so we can poll g_running */
    fcntl(srv, F_SETFL, O_NONBLOCK);

    while (g_running) {
        struct sockaddr_un cli_addr;
        socklen_t cli_len = sizeof(cli_addr);
        int cli = accept(srv, (struct sockaddr *)&cli_addr, &cli_len);
        if (cli < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(50000); /* 50ms poll */
                continue;
            }
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }

        /* Read command line (newline terminated) */
        char cmd[CMD_MAX] = {0};
        int total = 0;
        while (total < CMD_MAX - 1) {
            ssize_t r = read(cli, cmd + total, 1);
            if (r <= 0) break;
            if (cmd[total] == '\n') { cmd[total] = 0; break; }
            total++;
        }

        /* Parse: COMMAND [args...] */
        char *argv[32] = {0};
        int argc = 0;
        char *tok = strtok(cmd, " ");
        while (tok && argc < 31) { argv[argc++] = tok; tok = strtok(NULL, " "); }

        if (argc == 0) { close(cli); continue; }

        char resp[256];
        int rn;

        if (strcmp(argv[0], "start") == 0 && argc >= 4) {
            /* start <name> <rootfs_override|-> <cmd> [args...] */
            const char *cname  = argv[1];
            const char *croot  = strcmp(argv[2], "-") == 0 ? rootfs : argv[2];
            long soft = 0, hard = 0;
            /* optional: soft=N hard=N before cmd */
            int cmd_start = 3;
            if (argc > 5 && strncmp(argv[3], "soft=", 5) == 0) {
                soft = atol(argv[3] + 5);
                hard = atol(argv[4] + 5);
                cmd_start = 5;
            }
            char *cmd_argv[16] = {0};
            for (int i = cmd_start; i < argc && i - cmd_start < 15; i++)
                cmd_argv[i - cmd_start] = argv[i];
            if (do_start(cname, croot, cmd_argv, 0, soft, hard) == 0)
                rn = snprintf(resp, sizeof(resp), "OK: started %s\n", cname);
            else
                rn = snprintf(resp, sizeof(resp), "ERR: failed to start %s\n", cname);
            write(cli, resp, rn);

        } else if (strcmp(argv[0], "stop") == 0 && argc >= 2) {
            if (do_stop(argv[1]) == 0)
                rn = snprintf(resp, sizeof(resp), "OK: stopped %s\n", argv[1]);
            else
                rn = snprintf(resp, sizeof(resp), "ERR: could not stop %s\n", argv[1]);
            write(cli, resp, rn);

        } else if (strcmp(argv[0], "ps") == 0) {
            do_ps(cli);

        } else if (strcmp(argv[0], "logs") == 0 && argc >= 2) {
            do_logs(argv[1], cli);

        } else if (strcmp(argv[0], "shutdown") == 0) {
            rn = snprintf(resp, sizeof(resp), "OK: shutting down\n");
            write(cli, resp, rn);
            g_running = 0;

        } else {
            rn = snprintf(resp, sizeof(resp),
                          "ERR: unknown command '%s'\n", argv[0]);
            write(cli, resp, rn);
        }

        close(cli);
    }

    printf("[engine] Supervisor shutting down...\n");

    /* Send SIGTERM to all running containers */
    pthread_mutex_lock(&g_container_lock);
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        if (g_containers[i].used && g_containers[i].state == CS_RUNNING) {
            kill(g_containers[i].host_pid, SIGTERM);
        }
    }
    pthread_mutex_unlock(&g_container_lock);

    /* Give children time to exit */
    sleep(1);

    /* Reap remaining children */
    while (waitpid(-1, NULL, WNOHANG) > 0);

    /* Join logger threads */
    pthread_mutex_lock(&g_container_lock);
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        if (g_containers[i].used && g_containers[i].logger_running) {
            if (g_containers[i].pipe_write >= 0) {
                close(g_containers[i].pipe_write);
                g_containers[i].pipe_write = -1;
            }
        }
    }
    pthread_mutex_unlock(&g_container_lock);

    for (int i = 0; i < MAX_CONTAINERS; i++) {
        if (g_containers[i].used && g_containers[i].logger_running) {
            pthread_join(g_containers[i].logger_tid, NULL);
            g_containers[i].logger_running = 0;
        }
    }

    if (g_monitor_fd >= 0) close(g_monitor_fd);
    close(srv);
    unlink(SOCKET_PATH);
    printf("[engine] Supervisor exited cleanly.\n");
}

/* ------------------------------------------------------------------ */
/*  CLI client: send a command to the supervisor and print response     */
/* ------------------------------------------------------------------ */
static int cli_send(const char *cmd) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Cannot connect to supervisor at %s: %s\n"
                        "Is the supervisor running?\n",
                SOCKET_PATH, strerror(errno));
        close(fd);
        return 1;
    }

    /* Send command with newline */
    char buf[CMD_MAX];
    int n = snprintf(buf, sizeof(buf), "%s\n", cmd);
    write(fd, buf, n);

    /* Shutdown write side so server knows we're done */
    shutdown(fd, SHUT_WR);

    /* Read and print response */
    char rbuf[4096];
    ssize_t r;
    while ((r = read(fd, rbuf, sizeof(rbuf))) > 0)
        fwrite(rbuf, 1, r, stdout);

    close(fd);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  main                                                                */
/* ------------------------------------------------------------------ */
static void usage(const char *prog) {
    fprintf(stderr,
        "Usage:\n"
        "  %s supervisor <rootfs>                    - start supervisor\n"
        "  %s start <name> <rootfs|- > <cmd> [args]  - start container (background)\n"
        "  %s run   <name> <rootfs|-> <cmd> [args]   - run container (foreground)\n"
        "  %s ps                                     - list containers\n"
        "  %s logs  <name>                           - show container logs\n"
        "  %s stop  <name>                           - stop container\n"
        "  %s shutdown                               - stop supervisor\n",
        prog, prog, prog, prog, prog, prog, prog);
    exit(1);
}

int main(int argc, char *argv[]) {
    if (argc < 2) usage(argv[0]);

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) usage(argv[0]);
        supervisor_loop(argv[2]);
        return 0;
    }

    /* All other sub-commands: build a command string and send to supervisor */
    char cmd[CMD_MAX] = {0};
    int off = 0;
    for (int i = 1; i < argc && off < CMD_MAX - 2; i++) {
        if (i > 1) cmd[off++] = ' ';
        int r = snprintf(cmd + off, CMD_MAX - off, "%s", argv[i]);
        if (r > 0) off += r;
    }

    /* Special case: "run" is foreground – launch directly, not via socket */
    if (strcmp(argv[1], "run") == 0) {
        if (argc < 5) usage(argv[0]);
        const char *name   = argv[2];
        const char *rootfs = argv[3];
        char *cmd_argv[16] = {0};
        for (int i = 4; i < argc && i - 4 < 15; i++)
            cmd_argv[i - 4] = argv[i];
        /* Open monitor */
        g_monitor_fd = open(MONITOR_DEV, O_RDWR);
        return do_start(name, rootfs, cmd_argv, 1, 0, 0);
    }

    return cli_send(cmd);
}
