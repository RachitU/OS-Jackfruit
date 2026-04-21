#ifndef MONITOR_IOCTL_H
#define MONITOR_IOCTL_H

#include <linux/ioctl.h>

/* Magic number for this module */
#define MONITOR_MAGIC   0xCA

/*
 * struct container_info – payload for ioctl calls
 *
 * Passed from user space (engine) to kernel space (monitor).
 * soft_limit and hard_limit are in bytes; 0 means "not set".
 */
struct container_info {
    pid_t           pid;
    unsigned long   soft_limit;   /* bytes – warn threshold  */
    unsigned long   hard_limit;   /* bytes – kill threshold  */
};

/* Register a container PID with the monitor */
#define MONITOR_REGISTER    _IOW(MONITOR_MAGIC, 1, struct container_info)

/* Unregister a container PID */
#define MONITOR_UNREGISTER  _IOW(MONITOR_MAGIC, 2, struct container_info)

/* Query current RSS for a container PID */
#define MONITOR_QUERY_RSS   _IOWR(MONITOR_MAGIC, 3, struct container_info)

#endif /* MONITOR_IOCTL_H */
