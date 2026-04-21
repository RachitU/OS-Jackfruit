/*
 * monitor.c - Container Memory Monitor Kernel Module
 *
 * Provides /dev/container_monitor character device.
 * Accepts ioctl calls to:
 *   - Register a container PID with soft/hard memory limits
 *   - Unregister a container PID
 *   - Query current RSS of a container PID
 *
 * A kernel timer fires every POLL_INTERVAL_MS milliseconds.
 * On each tick it walks the registered list and:
 *   - Logs a warning if RSS > soft_limit (once per container)
 *   - Sends SIGKILL if RSS > hard_limit
 *
 * The registered list is protected by a mutex.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/timer.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/pid.h>
#include <linux/mm.h>
#include <linux/jiffies.h>
#include <linux/signal.h>
#include <linux/rcupdate.h>

#include "monitor_ioctl.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("OS-Jackfruit Team");
MODULE_DESCRIPTION("Container Memory Monitor");
MODULE_VERSION("1.0");

/* ------------------------------------------------------------------ */
/*  Configuration                                                       */
/* ------------------------------------------------------------------ */
#define DEVICE_NAME      "container_monitor"
#define CLASS_NAME       "container_monitor"
#define POLL_INTERVAL_MS 1000   /* check RSS every 1 s */

/* ------------------------------------------------------------------ */
/*  Per-container tracking entry                                        */
/* ------------------------------------------------------------------ */
struct tracked_container {
    pid_t           pid;
    unsigned long   soft_limit;   /* bytes */
    unsigned long   hard_limit;   /* bytes */
    int             soft_warned;  /* 1 if we already issued the soft warning */
    struct list_head list;
};

/* ------------------------------------------------------------------ */
/*  Module globals                                                      */
/* ------------------------------------------------------------------ */
static dev_t            g_devno;
static struct cdev      g_cdev;
static struct class    *g_class;
static struct device   *g_device;

static LIST_HEAD(g_container_list);
static DEFINE_MUTEX(g_list_mutex);

static struct timer_list g_poll_timer;

/* ------------------------------------------------------------------ */
/*  RSS helper: read VmRSS from /proc/<pid>/status via task struct     */
/* ------------------------------------------------------------------ */
static unsigned long get_rss_bytes(pid_t pid) {
    struct task_struct *task;
    unsigned long rss = 0;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (task && task->mm) {
        /* get_mm_rss returns pages */
        rss = get_mm_rss(task->mm) << PAGE_SHIFT;
    }
    rcu_read_unlock();
    return rss;
}

/* ------------------------------------------------------------------ */
/*  Send signal to a PID                                                */
/* ------------------------------------------------------------------ */
static void kill_container(pid_t pid, int sig) {
    struct task_struct *task;
    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (task)
        send_sig(sig, task, 0);
    rcu_read_unlock();
}

/* ------------------------------------------------------------------ */
/*  Timer callback: walk list and enforce limits                        */
/* ------------------------------------------------------------------ */
static void poll_timer_cb(struct timer_list *t) {
    struct tracked_container *entry, *tmp;

    mutex_lock(&g_list_mutex);
    list_for_each_entry_safe(entry, tmp, &g_container_list, list) {
        /* Check if process still alive */
        struct task_struct *task;
        rcu_read_lock();
        task = pid_task(find_vpid(entry->pid), PIDTYPE_PID);
        rcu_read_unlock();

        if (!task) {
            pr_info("[monitor] PID %d gone – removing from list\n", entry->pid);
            list_del(&entry->list);
            kfree(entry);
            continue;
        }

        unsigned long rss = get_rss_bytes(entry->pid);

        /* Hard limit enforcement */
        if (entry->hard_limit > 0 && rss > entry->hard_limit) {
            pr_warn("[monitor] PID %d RSS %lu KB > hard limit %lu KB – killing\n",
                    entry->pid,
                    rss / 1024,
                    entry->hard_limit / 1024);
            kill_container(entry->pid, SIGKILL);
            list_del(&entry->list);
            kfree(entry);
            continue;
        }

        /* Soft limit warning (only once per container) */
        if (entry->soft_limit > 0 && rss > entry->soft_limit &&
            !entry->soft_warned) {
            pr_warn("[monitor] PID %d RSS %lu KB > soft limit %lu KB – WARNING\n",
                    entry->pid,
                    rss / 1024,
                    entry->soft_limit / 1024);
            entry->soft_warned = 1;
        }
    }
    mutex_unlock(&g_list_mutex);

    /* Re-arm timer */
    mod_timer(&g_poll_timer, jiffies + msecs_to_jiffies(POLL_INTERVAL_MS));
}

/* ------------------------------------------------------------------ */
/*  ioctl handler                                                       */
/* ------------------------------------------------------------------ */
static long monitor_ioctl(struct file *filp, unsigned int cmd,
                           unsigned long arg) {
    struct container_info ci;

    if (copy_from_user(&ci, (void __user *)arg, sizeof(ci)))
        return -EFAULT;

    switch (cmd) {
    case MONITOR_REGISTER: {
        /* Check for duplicate */
        struct tracked_container *entry;
        mutex_lock(&g_list_mutex);
        list_for_each_entry(entry, &g_container_list, list) {
            if (entry->pid == ci.pid) {
                /* Update limits */
                entry->soft_limit   = ci.soft_limit;
                entry->hard_limit   = ci.hard_limit;
                entry->soft_warned  = 0;
                mutex_unlock(&g_list_mutex);
                pr_info("[monitor] Updated PID %d limits\n", ci.pid);
                return 0;
            }
        }

        struct tracked_container *tc = kzalloc(sizeof(*tc), GFP_KERNEL);
        if (!tc) {
            mutex_unlock(&g_list_mutex);
            return -ENOMEM;
        }
        tc->pid        = ci.pid;
        tc->soft_limit = ci.soft_limit;
        tc->hard_limit = ci.hard_limit;
        tc->soft_warned = 0;
        list_add_tail(&tc->list, &g_container_list);
        mutex_unlock(&g_list_mutex);
        pr_info("[monitor] Registered PID %d soft=%lu hard=%lu bytes\n",
                ci.pid, ci.soft_limit, ci.hard_limit);
        return 0;
    }

    case MONITOR_UNREGISTER: {
        struct tracked_container *entry, *tmp;
        mutex_lock(&g_list_mutex);
        list_for_each_entry_safe(entry, tmp, &g_container_list, list) {
            if (entry->pid == ci.pid) {
                list_del(&entry->list);
                kfree(entry);
                pr_info("[monitor] Unregistered PID %d\n", ci.pid);
                break;
            }
        }
        mutex_unlock(&g_list_mutex);
        return 0;
    }

    case MONITOR_QUERY_RSS: {
        unsigned long rss = get_rss_bytes(ci.pid);
        /* We repurpose soft_limit as the out-field for RSS */
        ci.soft_limit = rss;
        if (copy_to_user((void __user *)arg, &ci, sizeof(ci)))
            return -EFAULT;
        return 0;
    }

    default:
        return -ENOTTY;
    }
}

/* ------------------------------------------------------------------ */
/*  File operations                                                     */
/* ------------------------------------------------------------------ */
static int monitor_open(struct inode *inode, struct file *filp) {
    return 0;
}

static int monitor_release(struct inode *inode, struct file *filp) {
    return 0;
}

static const struct file_operations monitor_fops = {
    .owner          = THIS_MODULE,
    .open           = monitor_open,
    .release        = monitor_release,
    .unlocked_ioctl = monitor_ioctl,
};

/* ------------------------------------------------------------------ */
/*  Module init / exit                                                  */
/* ------------------------------------------------------------------ */
static int __init monitor_init(void) {
    int ret;

    ret = alloc_chrdev_region(&g_devno, 0, 1, DEVICE_NAME);
    if (ret < 0) {
        pr_err("[monitor] alloc_chrdev_region failed: %d\n", ret);
        return ret;
    }

    cdev_init(&g_cdev, &monitor_fops);
    g_cdev.owner = THIS_MODULE;
    ret = cdev_add(&g_cdev, g_devno, 1);
    if (ret < 0) {
        pr_err("[monitor] cdev_add failed: %d\n", ret);
        unregister_chrdev_region(g_devno, 1);
        return ret;
    }

    g_class = class_create(THIS_MODULE, CLASS_NAME);
    if (IS_ERR(g_class)) {
        ret = PTR_ERR(g_class);
        pr_err("[monitor] class_create failed: %d\n", ret);
        cdev_del(&g_cdev);
        unregister_chrdev_region(g_devno, 1);
        return ret;
    }

    g_device = device_create(g_class, NULL, g_devno, NULL, DEVICE_NAME);
    if (IS_ERR(g_device)) {
        ret = PTR_ERR(g_device);
        pr_err("[monitor] device_create failed: %d\n", ret);
        class_destroy(g_class);
        cdev_del(&g_cdev);
        unregister_chrdev_region(g_devno, 1);
        return ret;
    }

    /* Arm the periodic poll timer */
    timer_setup(&g_poll_timer, poll_timer_cb, 0);
    mod_timer(&g_poll_timer, jiffies + msecs_to_jiffies(POLL_INTERVAL_MS));

    pr_info("[monitor] Module loaded. Device: /dev/%s\n", DEVICE_NAME);
    return 0;
}

static void __exit monitor_exit(void) {
    del_timer_sync(&g_poll_timer);

    /* Free tracked list */
    struct tracked_container *entry, *tmp;
    mutex_lock(&g_list_mutex);
    list_for_each_entry_safe(entry, tmp, &g_container_list, list) {
        list_del(&entry->list);
        kfree(entry);
    }
    mutex_unlock(&g_list_mutex);

    device_destroy(g_class, g_devno);
    class_destroy(g_class);
    cdev_del(&g_cdev);
    unregister_chrdev_region(g_devno, 1);
    pr_info("[monitor] Module unloaded.\n");
}

module_init(monitor_init);
module_exit(monitor_exit);
