// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/debugfs.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/sysfs.h>
#include <linux/uaccess.h>

/*
 * --------------------------------------------------------------------------
 * THE SAME TWO VALUES, EXPOSED TWO WAYS
 * --------------------------------------------------------------------------
 *
 * `counter` and `enabled` are shared, plain module state - nothing about
 * them is debugfs- or sysfs-specific. What differs entirely is how much
 * code each interface needs to expose them, and what guarantees each one
 * makes to whoever is reading/writing:
 *
 *   sysfs (/sys/kernel/debugfs_sysfs_demo/)
 *     Part of the kernel's userspace ABI (see Documentation/ABI in
 *     ../../linux_mainline): one value per file, and once a sysfs
 *     attribute ships, the kernel is committed to not breaking whatever
 *     userspace grew to depend on it. Every attribute here is a
 *     hand-written show()/store() pair - full control over validation
 *     and side effects, at the cost of writing that code yourself.
 *
 *   debugfs (/sys/kernel/debug/debugfs_sysfs_demo/)
 *     Explicitly NOT an ABI. Documentation/filesystems/debugfs.rst says
 *     so directly: files can be added, renamed, or removed between
 *     kernel versions with no deprecation process, and no script should
 *     ever depend on one existing. In exchange, debugfs_create_u32()/
 *     debugfs_create_bool() below bind directly to a variable's address
 *     - no show(), no store(), no validation, one line each. That
 *     convenience is also the danger: writing to counter_raw here
 *     overwrites `counter` directly, no logging, no bounds, no
 *     semantics beyond "poke this memory."
 */

static u32 counter;
static bool enabled = true;
static DEFINE_MUTEX(data_lock);

/*
 * --------------------------------------------------------------------------
 * sysfs: /sys/kernel/debugfs_sysfs_demo/
 * --------------------------------------------------------------------------
 */

static ssize_t counter_show(struct kobject *kobj, struct kobj_attribute *attr,
			    char *buf)
{
	u32 value;

	mutex_lock(&data_lock);
	value = counter;
	mutex_unlock(&data_lock);

	return sysfs_emit(buf, "%u\n", value);
}

static struct kobj_attribute counter_attr = __ATTR_RO(counter);

static ssize_t enabled_show(struct kobject *kobj, struct kobj_attribute *attr,
			    char *buf)
{
	bool value;

	mutex_lock(&data_lock);
	value = enabled;
	mutex_unlock(&data_lock);

	return sysfs_emit(buf, "%d\n", value);
}

static ssize_t enabled_store(struct kobject *kobj,
			     struct kobj_attribute *attr, const char *buf,
			      size_t count)
{
	bool value;
	int ret;

	ret = kstrtobool(buf, &value);
	if (ret)
		return ret;

	mutex_lock(&data_lock);
	enabled = value;
	mutex_unlock(&data_lock);

	pr_info("sysfs: enabled=%d by %s[%d]\n", value, current->comm,
		current->pid);

	return count;
}

static struct kobj_attribute enabled_attr = __ATTR_RW(enabled);

static ssize_t increment_store(struct kobject *kobj,
			       struct kobj_attribute *attr, const char *buf,
				size_t count)
{
	u32 value;

	mutex_lock(&data_lock);
	counter++;
	value = counter;
	mutex_unlock(&data_lock);

	pr_info("sysfs: increment -> counter=%u by %s[%d]\n", value,
		current->comm, current->pid);

	return count;
}

static struct kobj_attribute increment_attr = __ATTR_WO(increment);

static struct attribute *sysfs_attrs[] = {
	&counter_attr.attr,
	&enabled_attr.attr,
	&increment_attr.attr,
	NULL
};

static const struct attribute_group sysfs_attr_group = {
	.attrs = sysfs_attrs,
};

static struct kobject *demo_kobj;

/*
 * --------------------------------------------------------------------------
 * debugfs: /sys/kernel/debug/debugfs_sysfs_demo/
 * --------------------------------------------------------------------------
 */

static ssize_t info_read(struct file *file, char __user *buf, size_t count,
			 loff_t *ppos)
{
	char kbuf[128];
	int len;
	u32 c;
	bool e;

	mutex_lock(&data_lock);
	c = counter;
	e = enabled;
	mutex_unlock(&data_lock);

	len = scnprintf(kbuf, sizeof(kbuf),
			"counter=%u\nenabled=%d\nNOTE: this file has no ABI stability guarantee\n",
			 c, e);

	return simple_read_from_buffer(buf, count, ppos, kbuf, len);
}

static const struct file_operations info_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = info_read,
};

static struct dentry *demo_debugfs_dir;

static int __init debugfs_sysfs_init(void)
{
	int ret;

	demo_kobj = kobject_create_and_add("debugfs_sysfs_demo", kernel_kobj);
	if (!demo_kobj)
		return -ENOMEM;

	ret = sysfs_create_group(demo_kobj, &sysfs_attr_group);
	if (ret) {
		kobject_put(demo_kobj);
		return ret;
	}

	/*
	 * debugfs_create_dir()/_u32()/_bool()/_file() are deliberately not
	 * checked for failure here. If CONFIG_DEBUG_FS is disabled, or
	 * debugfs failed to mount, these become harmless no-ops (some
	 * return NULL, some an ERR_PTR - the debugfs helpers all tolerate
	 * being handed either as a `parent`). A driver's actual
	 * functionality must never depend on debugfs existing; treating a
	 * missing debug/inspection interface as a load failure would be
	 * exactly backwards.
	 */
	demo_debugfs_dir = debugfs_create_dir("debugfs_sysfs_demo", NULL);

	debugfs_create_u32("counter_raw", 0644, demo_debugfs_dir, &counter);
	debugfs_create_bool("enabled_raw", 0644, demo_debugfs_dir, &enabled);
	debugfs_create_file("info", 0444, demo_debugfs_dir, NULL, &info_fops);

	pr_info("ready: /sys/kernel/debugfs_sysfs_demo/ and /sys/kernel/debug/debugfs_sysfs_demo/\n");

	return 0;
}

static void __exit debugfs_sysfs_exit(void)
{
	debugfs_remove_recursive(demo_debugfs_dir);

	sysfs_remove_group(demo_kobj, &sysfs_attr_group);
	kobject_put(demo_kobj);

	pr_info("unloaded: final counter=%u\n", counter);
}

module_init(debugfs_sysfs_init);
module_exit(debugfs_sysfs_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("guguali");
MODULE_DESCRIPTION("The same state exposed through sysfs (stable, hand-written) and debugfs (unstable, near-free)");
