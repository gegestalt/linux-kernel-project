// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/atomic.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/sysfs.h>
#include <linux/timer.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#define DRIVER_NAME "blocking_demo"

/*
 * How often the producer (a plain kernel timer - see lab 14 for timers in
 * depth) manufactures a new "event". Writable live, same as lab 04; a
 * change takes effect the next time the timer reschedules itself.
 */
static unsigned int interval_ms = 3000;
module_param(interval_ms, uint, 0644);
MODULE_PARM_DESC(interval_ms, "Milliseconds between produced events");

static DECLARE_WAIT_QUEUE_HEAD(event_wq);
static atomic_t data_ready = ATOMIC_INIT(0);
static atomic64_t event_id = ATOMIC64_INIT(0);
static atomic_t waiter_count = ATOMIC_INIT(0);

static struct timer_list producer_timer;

/*
 * --------------------------------------------------------------------------
 * PRODUCER
 * --------------------------------------------------------------------------
 *
 * Timer callbacks run in softirq context: no sleeping, no blocking
 * allocations, keep it short. All this one does is flip a flag, bump a
 * counter, and wake anyone waiting - exactly the kind of minimal, fast
 * hand-off a real interrupt handler would do before deferring the actual
 * work elsewhere.
 */
static void producer_fn(struct timer_list *t)
{
	atomic64_inc(&event_id);
	atomic_set(&data_ready, 1);

	wake_up_interruptible(&event_wq);

	mod_timer(&producer_timer,
		  jiffies + msecs_to_jiffies(READ_ONCE(interval_ms)));
}

/*
 * --------------------------------------------------------------------------
 * /dev/blocking_demo
 * --------------------------------------------------------------------------
 *
 * atomic_xchg(&data_ready, 0) atomically reads the flag *and* clears it
 * in one step. That matters with more than one reader blocked at once:
 * only whichever reader's xchg happens to run right after an event fires
 * will see a 1 and consume it; every other concurrently-woken reader sees
 * 0 and loops back to wait for the *next* event, rather than every reader
 * racing to read-then-clear separately and some of them seeing stale
 * state. Try it yourself with two `cat`s running at once (see the
 * README) and watch neither one ever double-consume an event.
 */
static ssize_t bq_read(struct file *file, char __user *buf, size_t count,
		       loff_t *ppos)
{
	char kbuf[64];
	int len;
	int ret;
	u64 id;

	for (;;) {
		if (atomic_xchg(&data_ready, 0))
			break;

		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;

		atomic_inc(&waiter_count);
		ret = wait_event_interruptible(event_wq,
					       atomic_read(&data_ready));
		atomic_dec(&waiter_count);

		if (ret)
			return ret;
	}

	id = atomic64_read(&event_id);

	len = scnprintf(kbuf, sizeof(kbuf), "event #%llu\n",
			(unsigned long long)id);

	return simple_read_from_buffer(buf, count, ppos, kbuf, len);
}

/*
 * poll_wait() registers this file on event_wq without blocking - the
 * VFS (via select()/poll()/epoll()) is the one that actually sleeps,
 * across potentially many file descriptors at once. This callback's job
 * is just: tell poll_wait() which queue to watch, then report current
 * readiness.
 */
static __poll_t bq_poll(struct file *file, struct poll_table_struct *wait)
{
	poll_wait(file, &event_wq, wait);

	if (atomic_read(&data_ready))
		return EPOLLIN | EPOLLRDNORM;

	return 0;
}

static const struct file_operations bq_fops = {
	.owner = THIS_MODULE,
	.read = bq_read,
	.poll = bq_poll,
};

static struct miscdevice bq_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = DRIVER_NAME,
	.fops = &bq_fops,
	.mode = 0444,
};

/*
 * --------------------------------------------------------------------------
 * sysfs
 * --------------------------------------------------------------------------
 */

static ssize_t event_id_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%llu\n",
			   (unsigned long long)atomic64_read(&event_id));
}

static DEVICE_ATTR_RO(event_id);

static ssize_t waiters_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	return sysfs_emit(buf, "%d\n", atomic_read(&waiter_count));
}

static DEVICE_ATTR_RO(waiters);

static ssize_t trigger_store(struct device *dev,
			     struct device_attribute *attr, const char *buf,
			      size_t count)
{
	atomic64_inc(&event_id);
	atomic_set(&data_ready, 1);

	wake_up_interruptible(&event_wq);

	pr_info("manual trigger by %s[%d]\n", current->comm, current->pid);

	return count;
}

static DEVICE_ATTR_WO(trigger);

static struct attribute *bq_attrs[] = {
	&dev_attr_event_id.attr,
	&dev_attr_waiters.attr,
	&dev_attr_trigger.attr,
	NULL
};

static const struct attribute_group bq_attr_group = {
	.attrs = bq_attrs,
};

static int __init wait_queues_blocking_init(void)
{
	int ret;

	ret = misc_register(&bq_miscdev);
	if (ret)
		return ret;

	ret = sysfs_create_group(&bq_miscdev.this_device->kobj,
				 &bq_attr_group);
	if (ret) {
		misc_deregister(&bq_miscdev);
		return ret;
	}

	timer_setup(&producer_timer, producer_fn, 0);
	mod_timer(&producer_timer,
		  jiffies + msecs_to_jiffies(interval_ms));

	pr_info("ready: /dev/%s interval_ms=%u\n", DRIVER_NAME, interval_ms);

	return 0;
}

static void __exit wait_queues_blocking_exit(void)
{
	/*
	 * timer_shutdown_sync() (the modern name for what older kernels
	 * called del_timer_sync()) both waits for any in-flight
	 * producer_fn() to finish and marks the timer as shut down, so
	 * even if producer_fn() itself is mid-flight right now and about
	 * to call mod_timer() to reschedule, that rearm is guaranteed not
	 * to happen. Getting this ordering wrong - freeing/unregistering
	 * before the timer is provably quiesced - is a classic
	 * use-after-free on module unload.
	 */
	timer_shutdown_sync(&producer_timer);

	/*
	 * In practice rmmod can never even reach this point while a
	 * reader is blocked in bq_read(): fops.owner = THIS_MODULE means
	 * every open /dev/blocking_demo file description holds a module
	 * reference for its entire lifetime, and rmmod refuses to run
	 * against a module with a nonzero refcount. This wake-up is
	 * defensive habit rather than something this specific driver
	 * needs - it matters for a wait queue fed by something that
	 * *isn't* covered by an open file's reference (a workqueue, another
	 * subsystem's callback, ...), where nothing else stops exit() from
	 * running out from under a still-blocked waiter.
	 */
	atomic_set(&data_ready, 1);
	wake_up_interruptible_all(&event_wq);

	sysfs_remove_group(&bq_miscdev.this_device->kobj, &bq_attr_group);
	misc_deregister(&bq_miscdev);

	pr_info("unloaded: final event_id=%llu\n",
		(unsigned long long)atomic64_read(&event_id));
}

module_init(wait_queues_blocking_init);
module_exit(wait_queues_blocking_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("guguali");
MODULE_DESCRIPTION("Blocking read with wait_event_interruptible() and poll() support");
