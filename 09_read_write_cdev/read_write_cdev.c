// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#define DEVICE_NAME "read_write_cdev"
#define BUF_SIZE 4096

/*
 * --------------------------------------------------------------------------
 * MODERN CHAR DEVICE REGISTRATION
 * --------------------------------------------------------------------------
 *
 * Labs 05 and 08 used register_chrdev(): one call, one dynamic major,
 * minors ignored, and a device node you have to mknod() by hand after
 * reading the major out of dmesg. That's fine for a two-line demo driver,
 * but it's not what real drivers do.
 *
 * The modern sequence is four separate steps, each with a narrower job:
 *
 *   alloc_chrdev_region()  reserve a (major, minor..minor+count) range
 *   cdev_init() + cdev_add()   bind a struct cdev (and its fops) to that range
 *   class_create()          register a device *class* in sysfs
 *   device_create()          create the actual /dev node under that class
 *
 * The payoff for the extra steps: device_create() makes udev create
 * /dev/read_write_cdev0 automatically the moment this module loads - no
 * manual mknod, no guessing the major number.
 */

static dev_t devt;
static struct cdev rw_cdev;
static struct class *rw_class;
static struct device *rw_device;

static char *buffer;
static size_t data_len;
static DEFINE_MUTEX(buf_lock);

static int rw_open(struct inode *inode, struct file *file)
{
	pr_info("open: ctx=%s[%d]\n", current->comm, current->pid);

	return 0;
}

static int rw_release(struct inode *inode, struct file *file)
{
	pr_info("release: ctx=%s[%d]\n", current->comm, current->pid);

	return 0;
}

/*
 * read() and write() both operate against the same fixed-size, in-kernel
 * buffer, addressed by the position the VFS tracks in *ppos (advanced by
 * every read/write, and independently seekable - see rw_llseek()).
 *
 * data_len is "how much of buffer holds data written so far", separate
 * from BUF_SIZE ("how much buffer exists"). read() can never go past
 * data_len; write() can never go past BUF_SIZE.
 */

static ssize_t rw_read(struct file *file, char __user *buf, size_t count,
		       loff_t *ppos)
{
	size_t available;
	size_t to_copy;

	mutex_lock(&buf_lock);

	if (*ppos < 0 || (size_t)*ppos >= data_len) {
		mutex_unlock(&buf_lock);
		return 0;
	}

	available = data_len - *ppos;
	to_copy = min(count, available);

	if (copy_to_user(buf, buffer + *ppos, to_copy)) {
		mutex_unlock(&buf_lock);
		return -EFAULT;
	}

	*ppos += to_copy;

	pr_info("read: bytes=%zu offset=%lld data_len=%zu ctx=%s[%d]\n",
		to_copy, *ppos, data_len, current->comm, current->pid);

	mutex_unlock(&buf_lock);

	return to_copy;
}

static ssize_t rw_write(struct file *file, const char __user *buf,
			size_t count, loff_t *ppos)
{
	size_t space;
	size_t to_copy;

	mutex_lock(&buf_lock);

	if (*ppos < 0 || (size_t)*ppos >= BUF_SIZE) {
		mutex_unlock(&buf_lock);
		return -ENOSPC;
	}

	space = BUF_SIZE - *ppos;
	to_copy = min(count, space);

	if (copy_from_user(buffer + *ppos, buf, to_copy)) {
		mutex_unlock(&buf_lock);
		return -EFAULT;
	}

	*ppos += to_copy;

	if ((size_t)*ppos > data_len)
		data_len = *ppos;

	pr_info("write: bytes=%zu offset=%lld data_len=%zu ctx=%s[%d]\n",
		to_copy, *ppos, data_len, current->comm, current->pid);

	mutex_unlock(&buf_lock);

	/*
	 * Short writes are legal per the write(2) contract, but returning
	 * less than `count` here only ever happens when the buffer is
	 * genuinely full (to_copy < count) - worth confirming with a write
	 * larger than BUF_SIZE and checking the caller's return value.
	 */
	return to_copy;
}

/*
 * fixed_size_llseek() is a generic helper for exactly this shape of
 * device: a fixed-capacity, randomly-addressable buffer. It implements
 * SEEK_SET/SEEK_CUR/SEEK_END against the size we pass it (BUF_SIZE) and
 * rejects anything that would move the position outside [0, BUF_SIZE].
 */
static loff_t rw_llseek(struct file *file, loff_t offset, int whence)
{
	return fixed_size_llseek(file, offset, whence, BUF_SIZE);
}

static const struct file_operations rw_fops = {
	.owner = THIS_MODULE,
	.open = rw_open,
	.release = rw_release,
	.read = rw_read,
	.write = rw_write,
	.llseek = rw_llseek,
};

static int __init read_write_cdev_init(void)
{
	int ret;

	buffer = kzalloc(BUF_SIZE, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;

	ret = alloc_chrdev_region(&devt, 0, 1, DEVICE_NAME);
	if (ret)
		goto err_free_buffer;

	cdev_init(&rw_cdev, &rw_fops);
	rw_cdev.owner = THIS_MODULE;

	ret = cdev_add(&rw_cdev, devt, 1);
	if (ret)
		goto err_unregister_region;

	rw_class = class_create(DEVICE_NAME);
	if (IS_ERR(rw_class)) {
		ret = PTR_ERR(rw_class);
		goto err_del_cdev;
	}

	rw_device = device_create(rw_class, NULL, devt, NULL, DEVICE_NAME "0");
	if (IS_ERR(rw_device)) {
		ret = PTR_ERR(rw_device);
		goto err_destroy_class;
	}

	pr_info("ready: /dev/%s0 major=%d minor=%d capacity=%d bytes\n",
		DEVICE_NAME, MAJOR(devt), MINOR(devt), BUF_SIZE);

	return 0;

err_destroy_class:
	class_destroy(rw_class);
err_del_cdev:
	cdev_del(&rw_cdev);
err_unregister_region:
	unregister_chrdev_region(devt, 1);
err_free_buffer:
	kfree(buffer);

	return ret;
}

static void __exit read_write_cdev_exit(void)
{
	device_destroy(rw_class, devt);
	class_destroy(rw_class);
	cdev_del(&rw_cdev);
	unregister_chrdev_region(devt, 1);
	kfree(buffer);

	pr_info("unloaded: final data_len=%zu\n", data_len);
}

module_init(read_write_cdev_init);
module_exit(read_write_cdev_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("guguali");
MODULE_DESCRIPTION("Modern cdev registration with a real seekable read/write buffer");
