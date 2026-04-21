/*
 * io_pulse.c - I/O-bound workload for scheduling experiments
 *
 * Usage: io_pulse [duration_seconds] [block_size_kb]
 *   Repeatedly writes and reads blocks to a temp file, sleeping briefly
 *   between ops to simulate an I/O-bound process with high block rates.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <signal.h>
#include <sys/stat.h>

#define TMP_FILE "/tmp/io_pulse_data"

static volatile int g_running = 1;
static void on_alarm(int sig) { (void)sig; g_running = 0; }

int main(int argc, char *argv[]) {
    int  duration   = 10;
    long block_kb   = 64;

    if (argc >= 2) duration = atoi(argv[1]);
    if (argc >= 3) block_kb = atol(argv[2]);

    printf("[io_pulse] Running for %d s, block=%ldKB (pid=%d)\n",
           duration, block_kb, getpid());
    fflush(stdout);

    signal(SIGALRM, on_alarm);
    alarm(duration);

    size_t bsz = (size_t)block_kb * 1024;
    char *buf = malloc(bsz);
    if (!buf) { perror("malloc"); return 1; }
    memset(buf, 0x55, bsz);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    unsigned long long ops = 0;

    while (g_running) {
        int fd = open(TMP_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) { perror("open"); break; }
        write(fd, buf, bsz);
        fsync(fd);
        close(fd);

        fd = open(TMP_FILE, O_RDONLY);
        if (fd >= 0) {
            read(fd, buf, bsz);
            close(fd);
        }
        ops++;
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double elapsed = (t1.tv_sec - t0.tv_sec) +
                     (t1.tv_nsec - t0.tv_nsec) / 1e9;

    printf("[io_pulse] %llu I/O round-trips in %.3f s (%.2f ops/s)\n",
           ops, elapsed, (double)ops / elapsed);
    fflush(stdout);

    free(buf);
    unlink(TMP_FILE);
    return 0;
}
