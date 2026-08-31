// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/init.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/uaccess.h>

#define PROC_DIR_NAME "procfs_demo"
#define MAX_EVENTS 64

/*
 * --------------------------------------------------------------------------
 * EVENT LOG
 * --------------------------------------------------------------------------
 *
 * Every open() of /proc/procfs_demo/events appends one record here before
 * serving the current contents back - opening the file is itself the
 * thing being logged, so cat-ing it twice in a row visibly grows the log
 * by one entry each time.
 *
 * The array is a simple bounded log, not a ring buffer: once MAX_EVENTS
 * is reached, further opens still increment total_opens but stop being
 * individually recorded, until a write of "clear" resets event_count.
 * That keeps the seq_file iteration below a plain array walk - a real
 * ring buffer's wraparound indexing is a distraction from what this lab
 * is isolating.
 */

struct proc_event {
	u64 ns;
	pid_t pid;
	char comm[TASK_COMM_LEN];
};

static struct proc_event events[MAX_EVENTS];
static unsigned int event_count;
static u64 total_opens;
static u64 loaded_at_ns;
static DEFINE_MUTEX(events_lock);

static void record_event(void)
{
	struct proc_event *ev;

	mutex_lock(&events_lock);

	total_opens++;

	if (event_count < MAX_EVENTS) {
		ev = &events[event_count++];
		ev->ns = ktime_get_ns();
		ev->pid = current->pid;
		get_task_comm(ev->comm, current);
	}

	mutex_unlock(&events_lock);
}

/*
 * --------------------------------------------------------------------------
 * /proc/procfs_demo/info - a single-value file
 * --------------------------------------------------------------------------
 *
 * proc_create_single() is the shortest path from "one show() function" to
 * a working /proc file: it wraps a bare seq_file show callback with a
 * trivial single-record iterator for you. Reach for this whenever there's
 * exactly one thing to print and no list to walk.
 */

static int info_show(struct seq_file *s, void *v)
{
	u64 uptime_ms;

	uptime_ms = (ktime_get_ns() - loaded_at_ns) / 1000000ULL;

	mutex_lock(&events_lock);
	seq_printf(s, "uptime_ms=%llu\n", uptime_ms);
	seq_printf(s, "total_opens=%llu\n", total_opens);
	seq_printf(s, "events_recorded=%u (capacity %u)\n", event_count,
		   MAX_EVENTS);
	mutex_unlock(&events_lock);

	return 0;
}

/*
 * --------------------------------------------------------------------------
 * /proc/procfs_demo/events - a multi-record listing
 * --------------------------------------------------------------------------
 *
 * This is the full seq_file iterator contract, the same one procfs uses
 * internally for things like /proc/PID/maps. The kernel calls these four
 * in this order for every single read() (not once per file lifetime):
 *
 *   start(pos)         acquire whatever protects the data, return the
 *                       element at *pos, or NULL if there is none
 *   show(element)       seq_printf() one element's worth of output
 *   next(element, pos)   advance *pos, return the next element or NULL
 *   ... show()/next() repeat until next() returns NULL, or the output
 *       buffer seq_read() was given fills up (in which case the kernel
 *       calls stop(), and on the *next* read() call, start() again with
 *       *pos where it left off) ...
 *   stop(element)        release whatever start() acquired
 *
 * stop() is always called to match start(), even on error paths and even
 * when start() itself returned NULL - it is the one function guaranteed
 * to run, which is why the lock is taken in start() and dropped in stop()
 * rather than per-element in show().
 */

static void *events_seq_start(struct seq_file *s, loff_t *pos)
{
	mutex_lock(&events_lock);

	if (*pos >= event_count)
		return NULL;

	return &events[*pos];
}

static void *events_seq_next(struct seq_file *s, void *v, loff_t *pos)
{
	(*pos)++;

	if (*pos >= event_count)
		return NULL;

	return &events[*pos];
}

static void events_seq_stop(struct seq_file *s, void *v)
{
	mutex_unlock(&events_lock);
}

static int events_seq_show(struct seq_file *s, void *v)
{
	struct proc_event *ev = v;
	u64 sec = ev->ns / 1000000000ULL;
	u64 usec = (ev->ns % 1000000000ULL) / 1000ULL;

	seq_printf(s, "%llu.%06llu %s[%d]\n", sec, usec, ev->comm, ev->pid);

	return 0;
}

static const struct seq_operations events_seq_ops = {
	.start = events_seq_start,
	.next = events_seq_next,
	.stop = events_seq_stop,
	.show = events_seq_show,
};

static int events_open(struct inode *inode, struct file *file)
{
	record_event();

	return seq_open(file, &events_seq_ops);
}

static ssize_t events_write(struct file *file, const char __user *buf,
			    size_t count, loff_t *ppos)
{
	char kbuf[16];

	if (count == 0 || count >= sizeof(kbuf))
		return -E2BIG;

	if (copy_from_user(kbuf, buf, count))
		return -EFAULT;

	kbuf[count] = '\0';

	if (!sysfs_streq(strim(kbuf), "clear"))
		return -EINVAL;

	mutex_lock(&events_lock);
	event_count = 0;
	mutex_unlock(&events_lock);

	pr_info("event log cleared by %s[%d]\n", current->comm, current->pid);

	return count;
}

static const struct proc_ops events_proc_ops = {
	.proc_open = events_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = seq_release,
	.proc_write = events_write,
};

/*
 * --------------------------------------------------------------------------
 * MODULE INIT / EXIT
 * --------------------------------------------------------------------------
 */

static struct proc_dir_entry *proc_dir;

static int __init procfs_seqfile_init(void)
{
	struct proc_dir_entry *info_entry;
	struct proc_dir_entry *events_entry;

	loaded_at_ns = ktime_get_ns();

	proc_dir = proc_mkdir(PROC_DIR_NAME, NULL);
	if (!proc_dir)
		return -ENOMEM;

	info_entry = proc_create_single("info", 0444, proc_dir, info_show);
	if (!info_entry)
		goto err_remove_dir;

	events_entry = proc_create("events", 0644, proc_dir, &events_proc_ops);
	if (!events_entry)
		goto err_remove_dir;

	pr_info("ready: /proc/%s/info /proc/%s/events\n", PROC_DIR_NAME,
		PROC_DIR_NAME);

	return 0;

err_remove_dir:
	proc_remove(proc_dir);

	return -ENOMEM;
}

static void __exit procfs_seqfile_exit(void)
{
	proc_remove(proc_dir);
	pr_info("unloaded: total_opens=%llu\n", total_opens);
}

module_init(procfs_seqfile_init);
module_exit(procfs_seqfile_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("guguali");
MODULE_DESCRIPTION("procfs single-value and seq_file multi-record listing experiment");
