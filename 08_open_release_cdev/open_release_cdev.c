// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/fs.h>
#include <linux/init.h>
#include <linux/module.h>

#define DEVICE_NAME "open_release_cdev"

static int major;

/*
 * Called by the VFS when a userspace process opens a device node
 * associated with this character device.
 */
static int my_open(struct inode *inode, struct file *filp)
{
	pr_info("open: Major=%u Minor=%u\n",
		imajor(inode), iminor(inode));

	pr_info("open: filp->f_pos   = %lld\n",
		(long long)filp->f_pos);

	pr_info("open: filp->f_mode  = 0x%x\n",
		(unsigned int)filp->f_mode);

	pr_info("open: filp->f_flags = 0x%x\n",
		filp->f_flags);

	return 0;
}

/*
 * Called when the userspace file descriptor is released.
 */
static int my_release(struct inode *inode, struct file *filp)
{
	pr_info("release: Major=%u Minor=%u f_pos=%lld\n",
		imajor(inode), iminor(inode),
		(long long)filp->f_pos);

	return 0;
}

/*
 * Connect VFS operations to our character-device callbacks.
 */
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

	pr_info("registered character device - Major Device Number: %d\n",
		major);

	return 0;
}

static void __exit open_release_cdev_exit(void)
{
	unregister_chrdev(major, DEVICE_NAME);

	pr_info("unregistered character device major=%d\n", major);
}

module_init(open_release_cdev_init);
module_exit(open_release_cdev_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("guguali");
MODULE_DESCRIPTION("Character device open/release and struct file experiment");
