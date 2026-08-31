// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/delay.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/kthread.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/sysfs.h>
#include <linux/uaccess.h>

#define DRIVER_NAME "kthread_demo"
#define RING_SIZE 16
#define SNAPSHOT_BUF_SIZE 640

/*
 * --------------------------------------------------------------------------
 * A DEDICATED PRODUCER THREAD
 * --------------------------------------------------------------------------
 *
 * Unlike lab 14's timer (softirq context, can't sleep) or lab 12's timer
 * driving a wait queue, this producer is a genuine, schedulable kernel
 * thread: kthread_run() below spawns it, it sleeps normally between
 * items with msleep_interruptible(), and it shuts down cooperatively
 * by noticing kthread_should_stop() rather than being killed.
 *
 * The /dev side is deliberately kept simple and non-blocking (each
 * read() drains whatever is currently buffered, or returns 0 if
 * nothing's arrived since the last read) - lab 12 is the place for
 * blocking read() semantics; this lab's whole point is the producer's
 * thread lifecycle, not the consumer side.
 */

static unsigned int interval_ms = 500;
module_param(interval_ms, uint, 0644);
MODULE_PARM_DESC(interval_ms, "Milliseconds between produced items");

struct ring_item {
	u64 seq;
	u64 ns;
};

static struct ring_item ring[RING_SIZE];
static unsigned int ring_head;
static unsigned int ring_count;
static DEFINE_MUTEX(ring_lock);

static u64 next_seq;
static u64 dropped_count;

static struct task_struct *producer_task;
static DEFINE_MUTEX(producer_task_lock);

static char snapshot_buf[SNAPSHOT_BUF_SIZE];
static int snapshot_len;

static int producer_thread_fn(void *data)
{
	pr_info("producer thread started: pid=%d comm=%s\n", current->pid,
		current->comm);

	while (!kthread_should_stop()) {
		mutex_lock(&ring_lock);

		ring[ring_head].seq = next_seq++;
		ring[ring_head].ns = ktime_get_ns();
		ring_head = (ring_head + 1) % RING_SIZE;

		if (ring_count < RING_SIZE)
			ring_count++;
		else
			dropped_count++;

		mutex_unlock(&ring_lock);

		/*
		 * msleep_interruptible() is what makes kthread_stop() below
		 * return promptly instead of waiting out a full interval:
		 * kthread_stop() wakes this task if it's sleeping, so the
		 * very next loop check sees kthread_should_stop() and exits
		 * cleanly rather than the thread being killed mid-sleep.
		 */
		msleep_interruptible(READ_ONCE(interval_ms));
	}

	pr_info("producer thread stopping: pid=%d produced=%llu dropped=%llu\n",
		current->pid, (unsigned long long)next_seq,
		(unsigned long long)dropped_count);

	return 0;
}

static int start_producer(void)
{
	int ret = 0;

	mutex_lock(&producer_task_lock);

	if (producer_task) {
		mutex_unlock(&producer_task_lock);
		return -EEXIST;
	}

	producer_task = kthread_run(producer_thread_fn, NULL, "kthread_demo_producer");
	if (IS_ERR(producer_task)) {
		ret = PTR_ERR(producer_task);
		producer_task = NULL;
	}

	mutex_unlock(&producer_task_lock);

	return ret;
}

static int stop_producer(void)
{
	struct task_struct *task;

	mutex_lock(&producer_task_lock);

	task = producer_task;
	producer_task = NULL;

	mutex_unlock(&producer_task_lock);

	if (!task)
		return -ENOENT;

	/*
	 * kthread_stop() sets the stop flag, wakes the task if it's
	 * sleeping, and blocks until producer_thread_fn() actually
	 * returns - so by the time this call returns, the thread is
	 * provably gone, not just "asked to leave".
	 */
	kthread_stop(task);

	return 0;
}

/*
 * --------------------------------------------------------------------------
 * /dev/kthread_demo - drain-on-read
 * --------------------------------------------------------------------------
 */

static ssize_t kthread_demo_read(struct file *file, char __user *buf,
				 size_t count, loff_t *ppos)
{
	struct ring_item items[RING_SIZE];
	unsigned int n, i, start;

	if (*ppos == 0) {
		mutex_lock(&ring_lock);

		n = ring_count;
		start = (ring_head + RING_SIZE - ring_count) % RING_SIZE;

		for (i = 0; i < n; i++)
			items[i] = ring[(start + i) % RING_SIZE];

		ring_count = 0;

		mutex_unlock(&ring_lock);

		snapshot_len = 0;
		for (i = 0; i < n && snapshot_len < SNAPSHOT_BUF_SIZE; i++) {
			snapshot_len += scnprintf(snapshot_buf + snapshot_len,
						   SNAPSHOT_BUF_SIZE - snapshot_len,
						   "seq=%llu ns=%llu\n",
						   (unsigned long long)items[i].seq,
						   (unsigned long long)items[i].ns);
		}
	}

	return simple_read_from_buffer(buf, count, ppos, snapshot_buf,
					snapshot_len);
}

static const struct file_operations kthread_demo_fops = {
	.owner = THIS_MODULE,
	.read = kthread_demo_read,
};

static struct miscdevice kthread_demo_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = DRIVER_NAME,
	.fops = &kthread_demo_fops,
	.mode = 0444,
};

/*
 * --------------------------------------------------------------------------
 * sysfs: /sys/kernel/kthreads_demo/
 * --------------------------------------------------------------------------
 */

static ssize_t status_show(struct kobject *kobj, struct kobj_attribute *attr,
			   char *buf)
{
	pid_t pid = -1;
	bool running;

	mutex_lock(&producer_task_lock);
	running = producer_task;
	if (running)
		pid = producer_task->pid;
	mutex_unlock(&producer_task_lock);

	return sysfs_emit(buf,
			   "running=%d\n"
			   "pid=%d\n"
			   "produced=%llu\n"
			   "dropped=%llu\n"
			   "buffered=%u\n",
			   running, pid, (unsigned long long)next_seq,
			   (unsigned long long)dropped_count, ring_count);
}

static struct kobj_attribute status_attr = __ATTR_RO(status);

static ssize_t control_store(struct kobject *kobj, struct kobj_attribute *attr,
			     const char *buf, size_t count)
{
	char cmd[16];
	int ret;

	if (count == 0 || count >= sizeof(cmd))
		return -E2BIG;

	memcpy(cmd, buf, count);
	cmd[count] = '\0';

	if (sysfs_streq(strim(cmd), "start"))
		ret = start_producer();
	else if (sysfs_streq(strim(cmd), "stop"))
		ret = stop_producer();
	else
		return -EINVAL;

	pr_info("control=%s by %s[%d] ret=%d\n", strim(cmd), current->comm,
		current->pid, ret);

	if (ret)
		return ret;

	return count;
}

static struct kobj_attribute control_attr = __ATTR_WO(control);

static struct attribute *kt_attrs[] = {
	&status_attr.attr,
	&control_attr.attr,
	NULL
};

static const struct attribute_group kt_attr_group = {
	.attrs = kt_attrs,
};

static struct kobject *kt_kobj;

static int __init kthreads_init(void)
{
	int ret;

	ret = misc_register(&kthread_demo_miscdev);
	if (ret)
		return ret;

	kt_kobj = kobject_create_and_add("kthreads_demo", kernel_kobj);
	if (!kt_kobj) {
		misc_deregister(&kthread_demo_miscdev);
		return -ENOMEM;
	}

	ret = sysfs_create_group(kt_kobj, &kt_attr_group);
	if (ret) {
		kobject_put(kt_kobj);
		misc_deregister(&kthread_demo_miscdev);
		return ret;
	}

	ret = start_producer();
	if (ret) {
		sysfs_remove_group(kt_kobj, &kt_attr_group);
		kobject_put(kt_kobj);
		misc_deregister(&kthread_demo_miscdev);
		return ret;
	}

	pr_info("ready: /dev/%s /sys/kernel/kthreads_demo/ interval_ms=%u\n",
		DRIVER_NAME, interval_ms);

	return 0;
}

static void __exit kthreads_exit(void)
{
	stop_producer();

	sysfs_remove_group(kt_kobj, &kt_attr_group);
	kobject_put(kt_kobj);
	misc_deregister(&kthread_demo_miscdev);

	pr_info("unloaded: produced=%llu dropped=%llu\n",
		(unsigned long long)next_seq,
		(unsigned long long)dropped_count);
}

module_init(kthreads_init);
module_exit(kthreads_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("guguali");
MODULE_DESCRIPTION("A producer kthread with cooperative shutdown via kthread_stop()");
