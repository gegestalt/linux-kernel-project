// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/atomic.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/string.h>

#define DEVICE_NAME "open_release_cdev"

/*
 * Per-open state.
 *
 * Each successful open() gets its own context and stores it in
 * struct file::private_data. release() receives the same struct file,
 * so it can recover information recorded at open time.
 */
struct open_context {
	u64 id;
	u64 opened_ns;
	unsigned int minor;
	unsigned int flags;
	fmode_t mode;
	pid_t opener_pid;
	char opener_comm[TASK_COMM_LEN];
};

static int major;

/* Number of currently active open file descriptions. */
static atomic_t active_opens = ATOMIC_INIT(0);

/* Monotonic ID assigned to each successful ->open() callback. */
static atomic64_t next_open_id = ATOMIC64_INIT(0);

static const char *access_mode_name(unsigned int flags)
{
	switch (flags & O_ACCMODE) {
	case O_RDONLY:
		return "O_RDONLY";
	case O_WRONLY:
		return "O_WRONLY";
	case O_RDWR:
		return "O_RDWR";
	default:
		return "UNKNOWN";
	}
}

/*
 * Called by the VFS when userspace opens a device node whose
 * major number resolves to this character device.
 */
static int my_open(struct inode *inode, struct file *filp)
{
	struct open_context *ctx;
	int active;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->id = atomic64_inc_return(&next_open_id);
	ctx->opened_ns = ktime_get_ns();
	ctx->minor = iminor(inode);
	ctx->flags = filp->f_flags;
	ctx->mode = filp->f_mode;
	ctx->opener_pid = current->pid;
	get_task_comm(ctx->opener_comm, current);

	filp->private_data = ctx;

	active = atomic_inc_return(&active_opens);

	pr_info("OPEN #%llu ----------------------------------------\n",
		(unsigned long long)ctx->id);

	pr_info("device: major=%u minor=%u\n",
		imajor(inode), iminor(inode));

	pr_info("process: %s[%d] active_opens=%d\n",
		current->comm, current->pid, active);

	/*
	 * These are the fields demonstrated by the original tutorial.
	 */
	pr_info("filp->f_pos   = %lld\n",
		(long long)filp->f_pos);

	pr_info("filp->f_mode  = 0x%x [read=%d write=%d]\n",
		(unsigned int)filp->f_mode,
		!!(filp->f_mode & FMODE_READ),
		!!(filp->f_mode & FMODE_WRITE));

	pr_info("filp->f_flags = 0x%x\n",
		filp->f_flags);

	/*
	 * Decode some useful userspace open() flags while keeping the
	 * raw hexadecimal value above for direct inspection.
	 */
	pr_info("flags: access=%s nonblock=%d append=%d sync=%d direct=%d\n",
		access_mode_name(filp->f_flags),
		!!(filp->f_flags & O_NONBLOCK),
		!!(filp->f_flags & O_APPEND),
		!!(filp->f_flags & O_SYNC),
		!!(filp->f_flags & O_DIRECT));

	pr_info("private_data: open_id=%llu recorded_minor=%u\n",
		(unsigned long long)ctx->id, ctx->minor);

	return 0;
}

/*
 * release() is called when the final reference to this open file
 * description is dropped.
 *
 * This is especially interesting with dup(): closing one duplicated
 * descriptor does not necessarily call ->release().
 */
static int my_release(struct inode *inode, struct file *filp)
{
	struct open_context *ctx = filp->private_data;
	u64 lifetime_ns;
	u64 lifetime_us;
	int active;

	if (!ctx) {
		pr_warn("release called without private_data\n");
		return 0;
	}

	lifetime_ns = ktime_get_ns() - ctx->opened_ns;
	lifetime_us = lifetime_ns / 1000ULL;

	active = atomic_dec_return(&active_opens);

	pr_info("RELEASE #%llu -------------------------------------\n",
		(unsigned long long)ctx->id);

	pr_info("device: major=%u minor=%u\n",
		imajor(inode), iminor(inode));

	pr_info("opened_by=%s[%d] released_by=%s[%d]\n",
		ctx->opener_comm, ctx->opener_pid,
		current->comm, current->pid);

	pr_info("lifetime=%llu us final_f_pos=%lld active_opens=%d\n",
		(unsigned long long)lifetime_us,
		(long long)filp->f_pos,
		active);

	kfree(ctx);
	filp->private_data = NULL;

	return 0;
}

static const struct file_operations fops = {
	.owner = THIS_MODULE,
	.open = my_open,
	.release = my_release,
};

static int __init open_release_cdev_init(void)
{
	major = register_chrdev(0, DEVICE_NAME, &fops);
	if (major < 0) {
		pr_err("register_chrdev failed: %d\n", major);
		return major;
	}

	pr_info("INIT ---------------------------------------------\n");
	pr_info("registered '%s' major=%d minors=0-255\n",
		DEVICE_NAME, major);
	pr_info("watch open/release callbacks, struct file state and lifetime\n");

	return 0;
}

static void __exit open_release_cdev_exit(void)
{
	pr_info("EXIT ---------------------------------------------\n");
	pr_info("active_opens=%d total_open_calls=%lld\n",
		atomic_read(&active_opens),
		(long long)atomic64_read(&next_open_id));

	unregister_chrdev(major, DEVICE_NAME);

	pr_info("unregistered '%s' major=%d\n",
		DEVICE_NAME, major);
}

module_init(open_release_cdev_init);
module_exit(open_release_cdev_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("guguali");
MODULE_DESCRIPTION("Character device open/release and struct file lifecycle experiment");
