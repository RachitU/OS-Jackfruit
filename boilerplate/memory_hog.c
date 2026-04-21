/*
 * memory_hog.c - Memory workload for container runtime testing
 *
 * Usage: memory_hog <target_mb> <step_mb> <delay_ms>
 *   Allocates <step_mb> every <delay_ms> ms until <target_mb> is reached.
 *   Touches every page to ensure RSS actually grows.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char *argv[]) {
    long target_mb = 100;
    long step_mb   = 10;
    long delay_ms  = 500;

    if (argc >= 2) target_mb = atol(argv[1]);
    if (argc >= 3) step_mb   = atol(argv[2]);
    if (argc >= 4) delay_ms  = atol(argv[3]);

    printf("[memory_hog] target=%ldMB step=%ldMB delay=%ldms\n",
           target_mb, step_mb, delay_ms);
    fflush(stdout);

    long allocated_mb = 0;
    while (allocated_mb < target_mb) {
        long chunk = step_mb * 1024 * 1024;
        char *p = malloc(chunk);
        if (!p) {
            fprintf(stderr, "[memory_hog] malloc failed at %ldMB\n", allocated_mb);
            break;
        }
        /* Touch every page so the OS actually maps it (RSS grows) */
        memset(p, 0xAA, chunk);
        allocated_mb += step_mb;
        printf("[memory_hog] Allocated %ldMB so far\n", allocated_mb);
        fflush(stdout);
        usleep((useconds_t)(delay_ms * 1000));
    }

    printf("[memory_hog] Done. Sleeping to keep RSS alive...\n");
    fflush(stdout);

    /* Hold the memory so the monitor can observe it */
    while (1) pause();
    return 0;
}
