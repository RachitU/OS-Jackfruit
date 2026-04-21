/*
 * cpu_hog.c - CPU-bound workload for scheduling experiments
 *
 * Usage: cpu_hog [duration_seconds]
 *   Spins on integer arithmetic for the given duration and prints
 *   iterations per second as a measure of CPU share received.
 */

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <signal.h>
#include <unistd.h>

static volatile int g_running = 1;

static void on_alarm(int sig) { (void)sig; g_running = 0; }

int main(int argc, char *argv[]) {
    int duration = 10;
    if (argc >= 2) duration = atoi(argv[1]);

    printf("[cpu_hog] Running for %d seconds (pid=%d)\n", duration, getpid());
    fflush(stdout);

    signal(SIGALRM, on_alarm);
    alarm(duration);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    unsigned long long iters = 0;
    unsigned long long dummy = 1;
    while (g_running) {
        /* Pure CPU work: integer operations */
        dummy = dummy * 6364136223846793005ULL + 1442695040888963407ULL;
        iters++;
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double elapsed = (t1.tv_sec - t0.tv_sec) +
                     (t1.tv_nsec - t0.tv_nsec) / 1e9;

    printf("[cpu_hog] Completed %llu iterations in %.3f s (%.2f Miter/s)\n",
           iters, elapsed, (double)iters / elapsed / 1e6);
    fflush(stdout);
    return 0;
}
