/*
 * monitor.c - Multi-Container Memory Monitor (Linux Kernel Module)
 *
 * Tasks implemented:
 *   Task 4 - Kernel memory monitoring with soft and hard limits
 *            - /dev/container_monitor character device
 *            - PID registration via ioctl (MONITOR_REGISTER / MONITOR_UNREGISTER)
 *            - Kernel linked list protected by a spinlock
 *            - Periodic RSS checks via a kernel timer (fires every second)
 *            - Soft-limit: one-shot warning logged to dmesg
 *            - Hard-limit: SIGKILL sent to the offending process
 *            - Stale/exited entries cleaned up automatically by the timer
 *            - All entries freed safely on module unload
 *
 * Synchronisation choice (justification for README):
 *   A spinlock is used to protect the monitored_list.  The timer callback
 *   runs in softirq context (bottom-half, non-preemptible) where sleeping
 *   is forbidden, so a mutex — which can sleep — is not usable on that path.
 *   The ioctl path runs in process context and can safely acquire a spinlock.
 *   The critical sections are short (list insert / delete, scalar reads), so
 *   busy-waiting overhead is negligible.
 */

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pid.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#include "monitor_ioctl.h"

#define DEVICE_NAME        "container_monitor"
#define CHECK_INTERVAL_SEC  1

/* ==============================================================
 * TODO 1: Linked-list node for one monitored container process.
 *
 * Fields:
 *   pid              - host PID registered by the supervisor
 *   container_id     - human-readable name (from ioctl request)
 *   soft_limit_bytes - threshold for the one-shot warning
 *   hard_limit_bytes - threshold that triggers SIGKILL
 *   soft_limit_warned- set to 1 after the first soft-limit event so
 *                      we emit the warning exactly once per container
 *   list             - kernel doubly-linked list linkage
 * ============================================================== */
struct monitored_entry {
    pid_t          pid;
    char           container_id[MONITOR_NAME_LEN];
    unsigned long  soft_limit_bytes;
    unsigned long  hard_limit_bytes;
    int            soft_limit_warned;   /* 1 after first soft-limit log */
    struct list_head list;
};

/* ==============================================================
 * TODO 2: Global list of monitored processes + protecting lock.
 *
 * monitored_list - the sentinel head; each node is a monitored_entry.
 * monitored_lock - spinlock chosen over mutex because the timer
 *                  callback runs in softirq context (cannot sleep).
 * ============================================================== */
static LIST_HEAD(monitored_list);
static DEFINE_SPINLOCK(monitored_lock);

/* --- Provided: internal device / timer state --- */
static struct timer_list monitor_timer;
static dev_t              dev_num;
static struct cdev        c_dev;
static struct class      *cl;

/* ---------------------------------------------------------------
 * Provided: RSS Helper
 *
 * Returns the Resident Set Size in bytes for the given PID,
 * or -1 if the task no longer exists.
 * --------------------------------------------------------------- */
static long get_rss_bytes(pid_t pid)
{
    struct task_struct *task;
    struct mm_struct   *mm;
    long rss_pages = 0;

    rcu_read_lock();
    task = pid_task(find_pid_ns(pid, &init_pid_ns), PIDTYPE_PID);
    if (!task) {
        rcu_read_unlock();
        return -1;
    }
    get_task_struct(task);
    rcu_read_unlock();

    mm = get_task_mm(task);
    if (mm) {
        rss_pages = get_mm_rss(mm);
        mmput(mm);
    }
    put_task_struct(task);

    return rss_pages * PAGE_SIZE;
}

/* ---------------------------------------------------------------
 * Provided: soft-limit helper
 * --------------------------------------------------------------- */
static void log_soft_limit_event(const char *container_id,
                                 pid_t       pid,
                                 unsigned long limit_bytes,
                                 long          rss_bytes)
{
    printk(KERN_WARNING
           "[container_monitor] SOFT LIMIT container=%s pid=%d"
           " rss=%ld limit=%lu\n",
           container_id, pid, rss_bytes, limit_bytes);
}

/* ---------------------------------------------------------------
 * Provided: hard-limit helper
 * --------------------------------------------------------------- */
static void kill_process(const char   *container_id,
                         pid_t         pid,
                         unsigned long limit_bytes,
                         long          rss_bytes)
{
    struct task_struct *task;

    rcu_read_lock();
    task = pid_task(find_pid_ns(pid, &init_pid_ns), PIDTYPE_PID);
    if (task)
        send_sig(SIGKILL, task, 1);
    rcu_read_unlock();

    printk(KERN_WARNING
           "[container_monitor] HARD LIMIT container=%s pid=%d"
           " rss=%ld limit=%lu — sent SIGKILL\n",
           container_id, pid, rss_bytes, limit_bytes);
}

/* ---------------------------------------------------------------
 * Timer Callback - fires every CHECK_INTERVAL_SEC seconds.
 * --------------------------------------------------------------- */
static void timer_callback(struct timer_list *t)
{
    /* ==============================================================
     * TODO 3: Periodic monitoring loop.
     *
     * Algorithm (all under the spinlock):
     *   For each entry in monitored_list:
     *     1. Call get_rss_bytes(). If -1 → process already gone →
     *        move entry to a local "to_free" list and skip.
     *     2. If rss >= hard_limit → kill, then mark for removal.
     *        Hard-limit check is done BEFORE soft so a process that
     *        blows straight past both limits is killed, not warned.
     *     3. Else if rss >= soft_limit AND not yet warned →
     *        log warning, set soft_limit_warned = 1.
     *
     * We collect entries that must be removed into a local list
     * while holding the spinlock (fast, no allocation), then free
     * them after releasing the lock so kfree() is never called
     * inside a spinlocked section.
     *
     * list_for_each_entry_safe() is required whenever we may delete
     * the current node inside the loop — it saves the "next" pointer
     * before the body runs.
     * ============================================================== */

    struct monitored_entry *entry, *tmp;
    LIST_HEAD(to_free);   /* local staging list for entries to delete */
    long rss;

    spin_lock(&monitored_lock);

    list_for_each_entry_safe(entry, tmp, &monitored_list, list) {

        rss = get_rss_bytes(entry->pid);

        /* Process has already exited — remove stale entry */
        if (rss < 0) {
            printk(KERN_INFO
                   "[container_monitor] container=%s pid=%d exited,"
                   " removing from watch list\n",
                   entry->container_id, entry->pid);
            list_del(&entry->list);
            list_add(&entry->list, &to_free);
            continue;
        }

        /* Hard-limit enforcement: kill and remove */
        if ((unsigned long)rss >= entry->hard_limit_bytes) {
            kill_process(entry->container_id, entry->pid,
                         entry->hard_limit_bytes, rss);
            list_del(&entry->list);
            list_add(&entry->list, &to_free);
            continue;
        }

        /* Soft-limit warning: emit once per container lifetime */
        if ((unsigned long)rss >= entry->soft_limit_bytes &&
            !entry->soft_limit_warned) {
            log_soft_limit_event(entry->container_id, entry->pid,
                                 entry->soft_limit_bytes, rss);
            entry->soft_limit_warned = 1;
        }
    }

    spin_unlock(&monitored_lock);

    /* Free collected entries outside the spinlock — kfree may not
     * be called while a spinlock is held on some kernel configs. */
    list_for_each_entry_safe(entry, tmp, &to_free, list) {
        list_del(&entry->list);
        kfree(entry);
    }

    /* Reschedule ourselves for the next check interval */
    mod_timer(&monitor_timer, jiffies + CHECK_INTERVAL_SEC * HZ);
}

/* ---------------------------------------------------------------
 * IOCTL Handler
 * --------------------------------------------------------------- */
static long monitor_ioctl(struct file *f,
                          unsigned int cmd,
                          unsigned long arg)
{
    struct monitor_request req;

    (void)f;

    if (cmd != MONITOR_REGISTER && cmd != MONITOR_UNREGISTER)
        return -EINVAL;

    if (copy_from_user(&req,
                       (struct monitor_request __user *)arg,
                       sizeof(req)))
        return -EFAULT;

    /* -----------------------------------------------
     * MONITOR_REGISTER
     * ----------------------------------------------- */
    if (cmd == MONITOR_REGISTER) {
        struct monitored_entry *entry;

        printk(KERN_INFO
               "[container_monitor] Registering container=%s pid=%d"
               " soft=%lu hard=%lu\n",
               req.container_id, req.pid,
               req.soft_limit_bytes, req.hard_limit_bytes);

        /* ==============================================================
         * TODO 4: Allocate and insert a new monitored_entry.
         *
         * Steps:
         *   1. Sanity-check: soft must not exceed hard.
         *   2. kzalloc a new entry (GFP_KERNEL — we are in process
         *      context here, sleeping is fine).
         *   3. Copy all fields from req into the entry.
         *   4. INIT_LIST_HEAD on the new node (belt-and-suspenders;
         *      kzalloc zeroes the memory, but explicit init is safer).
         *   5. Acquire spinlock, append to tail, release.
         * ============================================================== */

        if (req.soft_limit_bytes > req.hard_limit_bytes) {
            printk(KERN_WARNING
                   "[container_monitor] Register rejected:"
                   " soft(%lu) > hard(%lu)\n",
                   req.soft_limit_bytes, req.hard_limit_bytes);
            return -EINVAL;
        }

        entry = kzalloc(sizeof(*entry), GFP_KERNEL);
        if (!entry)
            return -ENOMEM;

        entry->pid               = req.pid;
        entry->soft_limit_bytes  = req.soft_limit_bytes;
        entry->hard_limit_bytes  = req.hard_limit_bytes;
        entry->soft_limit_warned = 0;
        strncpy(entry->container_id,
                req.container_id,
                MONITOR_NAME_LEN - 1);
        /* Guarantee NUL termination even if source is max-length */
        entry->container_id[MONITOR_NAME_LEN - 1] = '\0';
        INIT_LIST_HEAD(&entry->list);

        spin_lock(&monitored_lock);
        list_add_tail(&entry->list, &monitored_list);
        spin_unlock(&monitored_lock);

        return 0;
    }

    /* -----------------------------------------------
     * MONITOR_UNREGISTER
     * ----------------------------------------------- */
    printk(KERN_INFO
           "[container_monitor] Unregister request container=%s pid=%d\n",
           req.container_id, req.pid);

    /* ==============================================================
     * TODO 5: Find and remove a monitored entry on explicit unregister.
     *
     * Search criteria: match on BOTH pid AND container_id for safety
     * (prevents a misbehaving caller from unregistering a different
     * container's entry just by guessing a PID).
     *
     * Steps:
     *   1. Acquire spinlock.
     *   2. Walk the list with list_for_each_entry_safe.
     *   3. On match: list_del, release lock, kfree, return 0.
     *   4. If no match found: release lock, return -ENOENT.
     *
     * We release the lock before kfree so memory allocation/free
     * operations never execute inside the spinlock.
     * ============================================================== */
    {
        struct monitored_entry *entry, *tmp;
        struct monitored_entry *found = NULL;

        spin_lock(&monitored_lock);
        list_for_each_entry_safe(entry, tmp, &monitored_list, list) {
            if (entry->pid == req.pid &&
                strncmp(entry->container_id,
                        req.container_id,
                        MONITOR_NAME_LEN) == 0) {
                list_del(&entry->list);
                found = entry;
                break;
            }
        }
        spin_unlock(&monitored_lock);

        if (found) {
            kfree(found);
            return 0;
        }
    }

    return -ENOENT;
}

/* --- Provided: file operations --- */
static struct file_operations fops = {
    .owner          = THIS_MODULE,
    .unlocked_ioctl = monitor_ioctl,
};

/* --- Provided: Module Init --- */
static int __init monitor_init(void)
{
    if (alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME) < 0)
        return -1;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
    cl = class_create(DEVICE_NAME);
#else
    cl = class_create(THIS_MODULE, DEVICE_NAME);
#endif
    if (IS_ERR(cl)) {
        unregister_chrdev_region(dev_num, 1);
        return PTR_ERR(cl);
    }

    if (IS_ERR(device_create(cl, NULL, dev_num, NULL, DEVICE_NAME))) {
        class_destroy(cl);
        unregister_chrdev_region(dev_num, 1);
        return -1;
    }

    cdev_init(&c_dev, &fops);
    if (cdev_add(&c_dev, dev_num, 1) < 0) {
        device_destroy(cl, dev_num);
        class_destroy(cl);
        unregister_chrdev_region(dev_num, 1);
        return -1;
    }

    timer_setup(&monitor_timer, timer_callback, 0);
    mod_timer(&monitor_timer, jiffies + CHECK_INTERVAL_SEC * HZ);

    printk(KERN_INFO
           "[container_monitor] Module loaded. Device: /dev/%s\n",
           DEVICE_NAME);
    return 0;
}

/* --- Provided: Module Exit --- */
static void __exit monitor_exit(void)
{
    /* Stop the timer and wait for any in-progress callback to finish
     * before we tear down the list it may be iterating. */
    timer_shutdown_sync(&monitor_timer);

    /* ==============================================================
     * TODO 6: Free all remaining monitored entries on module unload.
     *
     * At this point the timer has been fully stopped (timer_shutdown_sync
     * guarantees no callback is running or will run again), so we can
     * walk and free the list without holding the spinlock.  We take it
     * anyway for correctness in case any ioctl call is still in flight
     * (unlikely after rmmod, but defensive).
     *
     * list_for_each_entry_safe is mandatory here because we delete
     * each node as we visit it.
     * ============================================================== */
    {
        struct monitored_entry *entry, *tmp;

        spin_lock(&monitored_lock);
        list_for_each_entry_safe(entry, tmp, &monitored_list, list) {
            list_del(&entry->list);
            kfree(entry);
        }
        spin_unlock(&monitored_lock);
    }

    cdev_del(&c_dev);
    device_destroy(cl, dev_num);
    class_destroy(cl);
    unregister_chrdev_region(dev_num, 1);

    printk(KERN_INFO "[container_monitor] Module unloaded.\n");
}

module_init(monitor_init);
module_exit(monitor_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Supervised multi-container memory monitor");
