// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/atomic.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/init.h>
#include <linux/minmax.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/sysfs.h>
#include <linux/uaccess.h>

#define DRIVER_NAME "race_demo"
#define MAX_INCREMENTS_PER_WRITE 1000000

/*
 * --------------------------------------------------------------------------
 * FOUR WAYS TO INCREMENT A SHARED COUNTER
 * --------------------------------------------------------------------------
 *
 * Every write() to /dev/race_demo increments a shared counter once per
 * byte written (the byte *contents* are never even read - only how many
 * bytes were requested matters, so `echo -n xxxxx` and a thread hammering
 * write() with the same count are equivalent). Which of the four modes
 * below actually does the incrementing is switchable at runtime through
 * sysfs, so the same stress test can be pointed at all four without
 * reloading the module.
 */

enum race_mode {
	MODE_NONE = 0,
	MODE_SPINLOCK = 1,
	MODE_MUTEX = 2,
	MODE_ATOMIC = 3,
};

static int mode = MODE_NONE;

static u64 counter_plain;
static atomic64_t counter_atomic = ATOMIC64_INIT(0);

static DEFINE_SPINLOCK(counter_spinlock);
static DEFINE_MUTEX(counter_mutex);

static u64 reset_count;

static void increment_once(void)
{
	u64 tmp;
	int i;

	switch (READ_ONCE(mode)) {
	case MODE_NONE:
		/*
		 * The textbook unsynchronized read-modify-write: read the
		 * shared value into a local, then write local+1 back. On
		 * real hardware this window is usually just a few
		 * instructions, so the race can be rare enough to hide
		 * under light load. The cpu_relax() loop below is not
		 * doing anything useful - it exists purely to hold this
		 * task in the middle of the race window long enough that
		 * concurrent writers reliably interleave with it, so the
		 * lost-update bug shows up on the first stress-test run
		 * instead of the hundredth.
		 */
		tmp = READ_ONCE(counter_plain);
		for (i = 0; i < 200; i++)
			cpu_relax();
		WRITE_ONCE(counter_plain, tmp + 1);
		break;

	case MODE_SPINLOCK:
		/*
		 * spin_lock() busy-waits rather than sleeping, so it may
		 * only ever be held somewhere that never sleeps. That's
		 * true here (the critical section is one increment), and
		 * it's cheap for short critical sections - no context
		 * switch either to acquire or while waiting.
		 */
		spin_lock(&counter_spinlock);
		counter_plain++;
		spin_unlock(&counter_spinlock);
		break;

	case MODE_MUTEX:
		/*
		 * mutex_lock() may sleep if the lock is contended, putting
		 * this task on a wait queue instead of spinning. Safe here
		 * because write() runs in ordinary process context, but it
		 * would be a bug to take this same mutex from an interrupt
		 * handler.
		 */
		mutex_lock(&counter_mutex);
		counter_plain++;
		mutex_unlock(&counter_mutex);
		break;

	case MODE_ATOMIC:
		/*
		 * No explicit lock at all: atomic64_inc() is a single
		 * hardware-guaranteed atomic read-modify-write instruction.
		 * Fastest of the three safe options, but only works because
		 * "increment by one" is the *entire* operation - anything
		 * that needs to combine more than one value atomically
		 * generally still needs a real lock.
		 */
		atomic64_inc(&counter_atomic);
		break;
	}
}

static u64 current_counter_value(void)
{
	if (READ_ONCE(mode) == MODE_ATOMIC)
		return atomic64_read(&counter_atomic);

	return READ_ONCE(counter_plain);
}

static void reset_counters(void)
{
	spin_lock(&counter_spinlock);
	counter_plain = 0;
	spin_unlock(&counter_spinlock);

	atomic64_set(&counter_atomic, 0);

	reset_count++;
}

/*
 * --------------------------------------------------------------------------
 * /dev/race_demo - the hot path
 * --------------------------------------------------------------------------
 */

static ssize_t race_write(struct file *file, const char __user *buf,
			  size_t count, loff_t *ppos)
{
	size_t n = min_t(size_t, count, MAX_INCREMENTS_PER_WRITE);
	size_t i;

	for (i = 0; i < n; i++)
		increment_once();

	return n;
}

static ssize_t race_read(struct file *file, char __user *buf, size_t count,
			 loff_t *ppos)
{
	char kbuf[32];
	int len;

	len = scnprintf(kbuf, sizeof(kbuf), "%llu\n",
			(unsigned long long)current_counter_value());

	return simple_read_from_buffer(buf, count, ppos, kbuf, len);
}

static const struct file_operations race_fops = {
	.owner = THIS_MODULE,
	.read = race_read,
	.write = race_write,
};

static struct miscdevice race_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = DRIVER_NAME,
	.fops = &race_fops,
	.mode = 0666,
};

/*
 * --------------------------------------------------------------------------
 * sysfs control plane
 * --------------------------------------------------------------------------
 */

static ssize_t mode_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	return sysfs_emit(buf, "%d\n", READ_ONCE(mode));
}

static ssize_t mode_store(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t count)
{
	int value;
	int ret;

	ret = kstrtoint(buf, 0, &value);
	if (ret)
		return ret;

	if (value < MODE_NONE || value > MODE_ATOMIC)
		return -EINVAL;

	WRITE_ONCE(mode, value);

	pr_info("mode=%d set by %s[%d]\n", value, current->comm,
		current->pid);

	return count;
}

static DEVICE_ATTR_RW(mode);

static ssize_t counter_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	return sysfs_emit(buf, "%llu\n",
			   (unsigned long long)current_counter_value());
}

static DEVICE_ATTR_RO(counter);

static ssize_t reset_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	reset_counters();

	pr_info("reset by %s[%d]\n", current->comm, current->pid);

	return count;
}

static DEVICE_ATTR_WO(reset);

static struct attribute *race_attrs[] = {
	&dev_attr_mode.attr,
	&dev_attr_counter.attr,
	&dev_attr_reset.attr,
	NULL
};

static const struct attribute_group race_attr_group = {
	.attrs = race_attrs,
};

static int __init concurrency_locking_init(void)
{
	int ret;

	ret = misc_register(&race_miscdev);
	if (ret)
		return ret;

	ret = sysfs_create_group(&race_miscdev.this_device->kobj,
				 &race_attr_group);
	if (ret) {
		misc_deregister(&race_miscdev);
		return ret;
	}

	pr_info("ready: /dev/%s mode=%d (0=none 1=spinlock 2=mutex 3=atomic)\n",
		DRIVER_NAME, mode);

	return 0;
}

static void __exit concurrency_locking_exit(void)
{
	sysfs_remove_group(&race_miscdev.this_device->kobj, &race_attr_group);
	misc_deregister(&race_miscdev);

	pr_info("unloaded: final counter=%llu resets=%llu\n",
		(unsigned long long)current_counter_value(),
		(unsigned long long)reset_count);
}

module_init(concurrency_locking_init);
module_exit(concurrency_locking_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("guguali");
MODULE_DESCRIPTION("A racy counter, then fixed three ways: spinlock, mutex, atomic_t");
