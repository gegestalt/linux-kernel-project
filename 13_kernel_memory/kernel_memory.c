// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>

#define CACHE_OBJ_SIZE 128

/*
 * --------------------------------------------------------------------------
 * THREE ALLOCATORS, ONE EXPERIMENT SLOT
 * --------------------------------------------------------------------------
 *
 * Only one allocation is tracked at a time - write "<type> <size>" to
 * allocate, "1" to free, and inspect what happened through `info` and
 * `stats` before freeing. This keeps the accounting trivial so the
 * allocator behavior itself stays the whole point.
 *
 *   kmalloc()   general-purpose slab allocator. Physically contiguous,
 *                so it's the only one of the three usable for DMA.
 *                Backed by fixed-size buckets - the actual usable size
 *                (via ksize()) is normally >= what you asked for. Has a
 *                hard ceiling (KMALLOC_MAX_SIZE, printed at load time)
 *                because it must find a *physically contiguous* run of
 *                pages, which gets harder to guarantee as size grows.
 *
 *   vmalloc()    allocates individual, possibly scattered physical pages
 *                and maps them into one *virtually* contiguous range.
 *                No practical size ceiling anywhere near kmalloc()'s, at
 *                the cost of a TLB/page-table setup on every allocation
 *                and no DMA-safety guarantee.
 *
 *   kmem_cache   a slab cache pre-configured for one fixed object size
 *                (CACHE_OBJ_SIZE here), created once at module load with
 *                kmem_cache_create(). Trades flexibility for speed: a
 *                driver that repeatedly allocates/frees many
 *                same-size objects (inodes, network buffers, ...) uses
 *                its own cache instead of the shared kmalloc buckets.
 */

enum alloc_type {
	ALLOC_NONE = 0,
	ALLOC_KMALLOC,
	ALLOC_VMALLOC,
	ALLOC_CACHE,
};

static const char *type_name(enum alloc_type type)
{
	switch (type) {
	case ALLOC_KMALLOC:
		return "kmalloc";
	case ALLOC_VMALLOC:
		return "vmalloc";
	case ALLOC_CACHE:
		return "cache";
	default:
		return "none";
	}
}

static struct kmem_cache *demo_cache;

static DEFINE_MUTEX(state_lock);
static enum alloc_type cur_type = ALLOC_NONE;
static void *cur_ptr;
static size_t cur_requested_size;
static size_t cur_actual_size;
static u64 cur_alloc_ns;
static u64 cur_free_ns;

static u64 stats_alloc_ok;
static u64 stats_alloc_fail;
static u64 stats_free_count;

static int do_allocate(enum alloc_type type, size_t size)
{
	u64 t0, t1;
	void *ptr;

	mutex_lock(&state_lock);

	if (cur_type != ALLOC_NONE) {
		mutex_unlock(&state_lock);
		return -EBUSY;
	}

	if (type == ALLOC_CACHE)
		size = CACHE_OBJ_SIZE;

	t0 = ktime_get_ns();

	switch (type) {
	case ALLOC_KMALLOC:
		ptr = kmalloc(size, GFP_KERNEL);
		break;
	case ALLOC_VMALLOC:
		ptr = vmalloc(size);
		break;
	case ALLOC_CACHE:
		ptr = kmem_cache_alloc(demo_cache, GFP_KERNEL);
		break;
	default:
		mutex_unlock(&state_lock);
		return -EINVAL;
	}

	t1 = ktime_get_ns();

	if (!ptr) {
		stats_alloc_fail++;
		mutex_unlock(&state_lock);
		pr_warn("%s(%zu) failed\n", type_name(type), size);
		return -ENOMEM;
	}

	cur_type = type;
	cur_ptr = ptr;
	cur_requested_size = size;
	cur_actual_size = (type == ALLOC_KMALLOC) ? ksize(ptr) : size;
	cur_alloc_ns = t1 - t0;
	stats_alloc_ok++;

	mutex_unlock(&state_lock);

	pr_info("%s: requested=%zu actual=%zu time=%llu ns by %s[%d]\n",
		type_name(type), size, cur_actual_size,
		(unsigned long long)cur_alloc_ns, current->comm,
		current->pid);

	return 0;
}

static int do_free(void)
{
	enum alloc_type type;
	void *ptr;
	u64 t0, t1;

	mutex_lock(&state_lock);

	if (cur_type == ALLOC_NONE) {
		mutex_unlock(&state_lock);
		return -ENOENT;
	}

	type = cur_type;
	ptr = cur_ptr;

	t0 = ktime_get_ns();

	switch (type) {
	case ALLOC_KMALLOC:
		kfree(ptr);
		break;
	case ALLOC_VMALLOC:
		vfree(ptr);
		break;
	case ALLOC_CACHE:
		kmem_cache_free(demo_cache, ptr);
		break;
	default:
		break;
	}

	t1 = ktime_get_ns();

	cur_free_ns = t1 - t0;
	stats_free_count++;
	cur_type = ALLOC_NONE;
	cur_ptr = NULL;

	mutex_unlock(&state_lock);

	pr_info("freed %s: time=%llu ns by %s[%d]\n", type_name(type),
		(unsigned long long)cur_free_ns, current->comm, current->pid);

	return 0;
}

/*
 * --------------------------------------------------------------------------
 * sysfs: /sys/kernel/kernel_memory/
 * --------------------------------------------------------------------------
 *
 * A bare kobject under kernel_kobj rather than a device or misc device -
 * there's no /dev node here because there's no read/write hot path to
 * expose, just a handful of control/inspection attributes. This is the
 * same mechanism (kobject + kobj_attribute, dispatched through
 * dynamic_kobj_ktype's generic sysfs_ops) that a plain "give me a top
 * level /sys/kernel/<name>/ directory" driver uses in the mainline tree.
 */

static ssize_t allocate_store(struct kobject *kobj,
			      struct kobj_attribute *attr, const char *buf,
			       size_t count)
{
	char kbuf[32];
	char *cmd, *size_str;
	unsigned long size;
	enum alloc_type type;
	int ret;

	if (count == 0 || count >= sizeof(kbuf))
		return -E2BIG;

	memcpy(kbuf, buf, count);
	kbuf[count] = '\0';
	cmd = strim(kbuf);

	size_str = strchr(cmd, ' ');
	if (!size_str)
		return -EINVAL;

	*size_str = '\0';
	size_str++;

	if (sysfs_streq(cmd, "kmalloc"))
		type = ALLOC_KMALLOC;
	else if (sysfs_streq(cmd, "vmalloc"))
		type = ALLOC_VMALLOC;
	else if (sysfs_streq(cmd, "cache"))
		type = ALLOC_CACHE;
	else
		return -EINVAL;

	ret = kstrtoul(strim(size_str), 0, &size);
	if (ret)
		return ret;

	ret = do_allocate(type, size);
	if (ret)
		return ret;

	return count;
}

static struct kobj_attribute allocate_attr = __ATTR_WO(allocate);

static ssize_t free_store(struct kobject *kobj, struct kobj_attribute *attr,
			  const char *buf, size_t count)
{
	int ret;

	ret = do_free();
	if (ret)
		return ret;

	return count;
}

static struct kobj_attribute free_attr = __ATTR_WO(free);

static ssize_t info_show(struct kobject *kobj, struct kobj_attribute *attr,
			 char *buf)
{
	enum alloc_type type;
	size_t requested, actual;
	u64 alloc_ns, free_ns;

	mutex_lock(&state_lock);
	type = cur_type;
	requested = cur_requested_size;
	actual = cur_actual_size;
	alloc_ns = cur_alloc_ns;
	free_ns = cur_free_ns;
	mutex_unlock(&state_lock);

	return sysfs_emit(buf,
			   "type=%s\n"
			   "requested_bytes=%zu\n"
			   "actual_bytes=%zu\n"
			   "last_alloc_ns=%llu\n"
			   "last_free_ns=%llu\n",
			   type_name(type), requested, actual,
			   (unsigned long long)alloc_ns,
			   (unsigned long long)free_ns);
}

static struct kobj_attribute info_attr = __ATTR_RO(info);

static ssize_t stats_show(struct kobject *kobj, struct kobj_attribute *attr,
			  char *buf)
{
	return sysfs_emit(buf,
			   "alloc_ok=%llu\n"
			   "alloc_fail=%llu\n"
			   "free_count=%llu\n"
			   "cache_obj_size=%d\n"
			   "kmalloc_max_size=%lu\n",
			   (unsigned long long)stats_alloc_ok,
			   (unsigned long long)stats_alloc_fail,
			   (unsigned long long)stats_free_count,
			   CACHE_OBJ_SIZE, (unsigned long)KMALLOC_MAX_SIZE);
}

static struct kobj_attribute stats_attr = __ATTR_RO(stats);

static struct attribute *km_attrs[] = {
	&allocate_attr.attr,
	&free_attr.attr,
	&info_attr.attr,
	&stats_attr.attr,
	NULL
};

static const struct attribute_group km_attr_group = {
	.attrs = km_attrs,
};

static struct kobject *km_kobj;

static int __init kernel_memory_init(void)
{
	int ret;

	demo_cache = kmem_cache_create("kernel_memory_demo", CACHE_OBJ_SIZE,
				       0, SLAB_HWCACHE_ALIGN, NULL);
	if (!demo_cache)
		return -ENOMEM;

	km_kobj = kobject_create_and_add("kernel_memory", kernel_kobj);
	if (!km_kobj) {
		kmem_cache_destroy(demo_cache);
		return -ENOMEM;
	}

	ret = sysfs_create_group(km_kobj, &km_attr_group);
	if (ret) {
		kobject_put(km_kobj);
		kmem_cache_destroy(demo_cache);
		return ret;
	}

	pr_info("ready: /sys/kernel/kernel_memory/ kmalloc_max_size=%lu cache_obj_size=%d\n",
		(unsigned long)KMALLOC_MAX_SIZE, CACHE_OBJ_SIZE);

	return 0;
}

static void __exit kernel_memory_exit(void)
{
	if (cur_type != ALLOC_NONE) {
		pr_warn("unloading with an active allocation still held - freeing it now\n");
		do_free();
	}

	sysfs_remove_group(km_kobj, &km_attr_group);
	kobject_put(km_kobj);
	kmem_cache_destroy(demo_cache);

	pr_info("unloaded: alloc_ok=%llu alloc_fail=%llu free_count=%llu\n",
		(unsigned long long)stats_alloc_ok,
		(unsigned long long)stats_alloc_fail,
		(unsigned long long)stats_free_count);
}

module_init(kernel_memory_init);
module_exit(kernel_memory_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("guguali");
MODULE_DESCRIPTION("kmalloc vs vmalloc vs kmem_cache, compared live through sysfs");
