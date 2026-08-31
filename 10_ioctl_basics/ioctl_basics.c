// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include "ioctl_basics.h"

#define DEVICE_NAME "ioctl_basics"

static dev_t devt;
static struct cdev ioctl_cdev;
static struct class *ioctl_class;
static struct device *ioctl_device;

static struct ioctl_basics_stats stats;
static DEFINE_MUTEX(state_lock);

static int ioctl_basics_open(struct inode *inode, struct file *file)
{
	return 0;
}

static void apply_mode_transform(char *buf, __u32 mode)
{
	size_t len = strlen(buf);
	size_t i;
	char tmp;

	switch (mode) {
	case IOCTL_BASICS_MODE_UPPER:
		for (i = 0; i < len; i++) {
			if (buf[i] >= 'a' && buf[i] <= 'z')
				buf[i] -= 'a' - 'A';
		}
		break;

	case IOCTL_BASICS_MODE_REVERSE:
		for (i = 0; i < len / 2; i++) {
			tmp = buf[i];
			buf[i] = buf[len - 1 - i];
			buf[len - 1 - i] = tmp;
		}
		break;

	case IOCTL_BASICS_MODE_IDENTITY:
	default:
		break;
	}
}

/*
 * unlocked_ioctl() receives the raw command number the caller passed to
 * ioctl(2) plus a single unsigned long argument, which is either a value
 * directly (rare) or, as in every case below, a userspace pointer that
 * must go through copy_to_user()/copy_from_user() just like read()/
 * write() would - ioctl() does not exempt this argument from the
 * kernel/userspace boundary.
 *
 * Any command number this switch doesn't recognize returns -ENOTTY -
 * the conventional "no such ioctl" error, borrowed from tty ioctls but
 * used generically across the kernel.
 */
static long ioctl_basics_ioctl(struct file *file, unsigned int cmd,
			       unsigned long arg)
{
	void __user *argp = (void __user *)arg;

	switch (cmd) {
	case IOCTL_BASICS_RESET:
		mutex_lock(&state_lock);
		stats.reads = 0;
		stats.echoes = 0;
		stats.mode = IOCTL_BASICS_MODE_IDENTITY;
		stats.resets++;
		mutex_unlock(&state_lock);

		pr_info("RESET by %s[%d]\n", current->comm, current->pid);

		return 0;

	case IOCTL_BASICS_GET_STATS: {
		struct ioctl_basics_stats snapshot;

		mutex_lock(&state_lock);
		stats.reads++;
		snapshot = stats;
		mutex_unlock(&state_lock);

		if (copy_to_user(argp, &snapshot, sizeof(snapshot)))
			return -EFAULT;

		return 0;
	}

	case IOCTL_BASICS_SET_MODE: {
		__u32 mode;

		if (copy_from_user(&mode, argp, sizeof(mode)))
			return -EFAULT;

		if (mode > IOCTL_BASICS_MODE_REVERSE)
			return -EINVAL;

		mutex_lock(&state_lock);
		stats.mode = mode;
		mutex_unlock(&state_lock);

		pr_info("SET_MODE mode=%u by %s[%d]\n", mode, current->comm,
			current->pid);

		return 0;
	}

	case IOCTL_BASICS_ECHO: {
		struct ioctl_basics_echo req;
		__u32 mode;

		if (copy_from_user(&req, argp, sizeof(req)))
			return -EFAULT;

		req.buf[sizeof(req.buf) - 1] = '\0';

		mutex_lock(&state_lock);
		mode = stats.mode;
		apply_mode_transform(req.buf, mode);
		stats.echoes++;
		mutex_unlock(&state_lock);

		pr_info("ECHO mode=%u by %s[%d]\n", mode, current->comm,
			current->pid);

		if (copy_to_user(argp, &req, sizeof(req)))
			return -EFAULT;

		return 0;
	}

	default:
		return -ENOTTY;
	}
}

static const struct file_operations ioctl_basics_fops = {
	.owner = THIS_MODULE,
	.open = ioctl_basics_open,
	.unlocked_ioctl = ioctl_basics_ioctl,
};

static int __init ioctl_basics_init(void)
{
	int ret;

	ret = alloc_chrdev_region(&devt, 0, 1, DEVICE_NAME);
	if (ret)
		return ret;

	cdev_init(&ioctl_cdev, &ioctl_basics_fops);
	ioctl_cdev.owner = THIS_MODULE;

	ret = cdev_add(&ioctl_cdev, devt, 1);
	if (ret)
		goto err_unregister_region;

	ioctl_class = class_create(DEVICE_NAME);
	if (IS_ERR(ioctl_class)) {
		ret = PTR_ERR(ioctl_class);
		goto err_del_cdev;
	}

	ioctl_device = device_create(ioctl_class, NULL, devt, NULL,
				     DEVICE_NAME "0");
	if (IS_ERR(ioctl_device)) {
		ret = PTR_ERR(ioctl_device);
		goto err_destroy_class;
	}

	pr_info("ready: /dev/%s0 major=%d minor=%d\n", DEVICE_NAME,
		MAJOR(devt), MINOR(devt));

	return 0;

err_destroy_class:
	class_destroy(ioctl_class);
err_del_cdev:
	cdev_del(&ioctl_cdev);
err_unregister_region:
	unregister_chrdev_region(devt, 1);

	return ret;
}

static void __exit ioctl_basics_exit(void)
{
	device_destroy(ioctl_class, devt);
	class_destroy(ioctl_class);
	cdev_del(&ioctl_cdev);
	unregister_chrdev_region(devt, 1);

	pr_info("unloaded: resets=%llu echoes=%llu\n",
		(unsigned long long)stats.resets,
		(unsigned long long)stats.echoes);
}

module_init(ioctl_basics_init);
module_exit(ioctl_basics_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("guguali");
MODULE_DESCRIPTION("Custom ioctl commands: _IO/_IOR/_IOW/_IOWR in practice");
