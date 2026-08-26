// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/ktime.h>
#include <linux/sched.h>
#include <linux/atomic.h>
#include <linux/errno.h>

#define DEVICE_NAME "register_cdev"

static int major;
static u64 loaded_at_ns;
static atomic_t open_count = ATOMIC_INIT(0);

/*
 * Called when userspace opens a device node whose major/minor
 * resolves to this character device.
 */
static int register_cdev_open(struct inode *inode, struct file *file)
{
	int count;

	count = atomic_inc_return(&open_count);

	pr_info("open: dev=%u:%u ctx=%s[%d] opens=%d inode=%p file=%p\\n",
		imajor(inode), iminor(inode), current->comm, current->pid,
		count, inode, file);

	return 0;
}

/*
 * Return information about the character device to userspace.
 *
 * The message is created in kernel memory and transferred to
 * the userspace read buffer using copy_to_user().
 */
static ssize_t register_cdev_read(struct file *file, char __user *buffer,
				  size_t count, loff_t *offset)
{
	char message[256];
	int message_length;
	size_t bytes_to_copy;
	unsigned int minor;

	minor = iminor(file_inode(file));

	/*
	 * message[] is stored on the kernel stack of the task
	 * currently executing this read callback.
	 */
	message_length = scnprintf(message, sizeof(message),
				   "register_cdev kernel device\\n"
				   "major=%d\\n"
				   "minor=%u\\n"
				   "context=%s[%d]\\n",
				   major, minor, current->comm, current->pid);

	/*
	 * Returning zero indicates EOF. This allows programs such
	 * as cat to stop after consuming the generated message.
	 */
	if (*offset >= message_length)
		return 0;

	bytes_to_copy = message_length - *offset;

	if (bytes_to_copy > count)
		bytes_to_copy = count;

	/*
	 * buffer is a userspace pointer, so the data must cross the
	 * kernel/userspace boundary through the uaccess API.
	 */
	if (copy_to_user(buffer, message + *offset, bytes_to_copy))
		return -EFAULT;

	*offset += bytes_to_copy;

	pr_info("read: dev=%d:%u bytes=%zu offset=%lld ctx=%s[%d]\\n",
		major, minor, bytes_to_copy, (long long)*offset,
		current->comm, current->pid);

	return bytes_to_copy;
}

/*
 * Called when the userspace file descriptor is released.
 */
static int register_cdev_release(struct inode *inode, struct file *file)
{
	int count;

	count = atomic_dec_return(&open_count);

	pr_info("release: dev=%u:%u ctx=%s[%d] opens=%d file=%p\\n",
		imajor(inode), iminor(inode), current->comm, current->pid,
		count, file);

	return 0;
}

/*
 * Connect VFS file operations to callbacks implemented by
 * this module.
 */
static const struct file_operations register_cdev_fops = {
	.owner = THIS_MODULE,
	.open = register_cdev_open,
	.read = register_cdev_read,
	.release = register_cdev_release,
};

/*
 * Register the character device.
 *
 * Passing zero as the requested major asks Linux to allocate
 * an available major number dynamically.
 */
static int __init register_cdev_init(void)
{
	u64 start_ns;
	u64 elapsed_ns;

	start_ns = ktime_get_ns();
	loaded_at_ns = start_ns;

	major = register_chrdev(0, DEVICE_NAME, &register_cdev_fops);
	if (major < 0) {
		pr_err("init: register_chrdev failed err=%d\\n", major);
		return major;
	}

	elapsed_ns = ktime_get_ns() - start_ns;

	pr_info("init: major=%d minors=0-255 ctx=%s[%d] time=%llu us\\n",
		major, current->comm, current->pid,
		(unsigned long long)(elapsed_ns / 1000ULL));

	/*
	 * Pointer values may be restricted or hashed by the kernel,
	 * but they still illustrate that these are distinct objects.
	 */
	pr_info("objects: module=%p fops=%p major=%p open_count=%p\\n",
		THIS_MODULE, &register_cdev_fops, &major, &open_count);

	return 0;
}

/*
 * Remove the character-device registration and report the
 * lifetime of this loaded module instance.
 */
static void __exit register_cdev_exit(void)
{
	u64 start_ns;
	u64 uptime_ns;
	u64 elapsed_ns;

	start_ns = ktime_get_ns();
	uptime_ns = start_ns - loaded_at_ns;

	pr_info("exit: major=%d opens=%d ctx=%s[%d] uptime=%llu ms\\n",
		major, atomic_read(&open_count), current->comm,
		current->pid,
		(unsigned long long)(uptime_ns / 1000000ULL));

	unregister_chrdev(major, DEVICE_NAME);

	elapsed_ns = ktime_get_ns() - start_ns;

	pr_info("exit: unregistered major=%d time=%llu us\\n",
		major, (unsigned long long)(elapsed_ns / 1000ULL));
}

module_init(register_cdev_init);
module_exit(register_cdev_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("guguali");
MODULE_DESCRIPTION("Character device registration, memory and read experiment");
