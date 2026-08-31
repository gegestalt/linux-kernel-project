// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/delay.h>
#include <linux/hardirq.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/preempt.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/sysfs.h>
#include <linux/timer.h>
#include <linux/workqueue.h>

/*
 * --------------------------------------------------------------------------
 * TWO HEARTBEATS, TWO EXECUTION CONTEXTS
 * --------------------------------------------------------------------------
 *
 * A struct timer_list and a delayed_work item, each rescheduling itself
 * forever at its own interval, each recording exactly what execution
 * context it ran in. They do the same job (tick a counter) so the only
 * real difference on display is *where* the kernel actually ran each one:
 *
 *   heartbeat_timer   runs from softirq context (run_timer_softirq()).
 *                       No sleeping, no blocking allocation, keep it
 *                       short - "current" here is whatever task
 *                       happened to be running when the softirq fired,
 *                       not a thread that belongs to this timer.
 *
 *   heartbeat_work    runs from a kworker *thread* - ordinary process
 *                       context, can sleep, can call GFP_KERNEL
 *                       allocations, can take a mutex. "current" is a
 *                       real, dedicated kworker task.
 */

static unsigned int timer_interval_ms = 1000;
module_param(timer_interval_ms, uint, 0644);
MODULE_PARM_DESC(timer_interval_ms, "Milliseconds between timer heartbeats");

static unsigned int work_interval_ms = 1000;
module_param(work_interval_ms, uint, 0644);
MODULE_PARM_DESC(work_interval_ms, "Milliseconds between workqueue heartbeats");

static struct timer_list heartbeat_timer;
static struct delayed_work heartbeat_work;

static DEFINE_SPINLOCK(stats_lock);

static u64 timer_ticks;
static char timer_last_ctx[128];

static u64 work_ticks;
static char work_last_ctx[128];

static void format_ctx(char *buf, size_t size)
{
	snprintf(buf, size,
		 "in_interrupt=%d in_softirq=%d in_task=%d preemptible=%d comm=%s pid=%d",
		 !!in_interrupt(), !!in_softirq(), !!in_task(), !!preemptible(),
		 current->comm, current->pid);
}

/*
 * Runs in softirq context. might_sleep() is the kernel's own debug
 * assertion for exactly this mistake: on a kernel built with
 * CONFIG_DEBUG_ATOMIC_SLEEP=y, calling it from a context that must not
 * sleep prints a "BUG: sleeping function called from invalid context"
 * warning (non-fatal - a WARN splat, not a panic). On a kernel without
 * that debug option (check yours: grep CONFIG_DEBUG_ATOMIC_SLEEP
 * /boot/config-$(uname -r)) it's a harmless no-op. Either way, it is
 * *only* the debug check here - this callback never actually calls a
 * sleeping function, so nothing breaks regardless of kernel config.
 */
static void heartbeat_timer_fn(struct timer_list *t)
{
	char ctx[128];

	might_sleep();

	format_ctx(ctx, sizeof(ctx));

	spin_lock(&stats_lock);
	timer_ticks++;
	strscpy(timer_last_ctx, ctx, sizeof(timer_last_ctx));
	spin_unlock(&stats_lock);

	mod_timer(&heartbeat_timer,
		  jiffies + msecs_to_jiffies(READ_ONCE(timer_interval_ms)));
}

/*
 * Runs in process context on a kworker thread. Unlike the timer above,
 * an actual sleeping call here is completely legal - usleep_range()
 * below proves it by not warning about anything.
 */
static void heartbeat_work_fn(struct work_struct *work)
{
	char ctx[128];

	/*
	 * msleep() below ~20ms can actually sleep for up to 20ms due to
	 * timer granularity - usleep_range() is the kernel's recommended
	 * way to sleep for a short, precise-ish duration instead.
	 */
	usleep_range(1000, 2000);

	format_ctx(ctx, sizeof(ctx));

	spin_lock(&stats_lock);
	work_ticks++;
	strscpy(work_last_ctx, ctx, sizeof(work_last_ctx));
	spin_unlock(&stats_lock);

	schedule_delayed_work(&heartbeat_work,
			      msecs_to_jiffies(READ_ONCE(work_interval_ms)));
}

/*
 * --------------------------------------------------------------------------
 * sysfs: /sys/kernel/timers_workqueues/stats
 * --------------------------------------------------------------------------
 */

static ssize_t stats_show(struct kobject *kobj, struct kobj_attribute *attr,
			  char *buf)
{
	u64 t_ticks, w_ticks;
	char t_ctx[128], w_ctx[128];

	spin_lock(&stats_lock);
	t_ticks = timer_ticks;
	w_ticks = work_ticks;
	strscpy(t_ctx, timer_last_ctx, sizeof(t_ctx));
	strscpy(w_ctx, work_last_ctx, sizeof(w_ctx));
	spin_unlock(&stats_lock);

	return sysfs_emit(buf,
			   "timer_ticks=%llu\n"
			   "timer_last_ctx: %s\n"
			   "work_ticks=%llu\n"
			   "work_last_ctx: %s\n",
			   (unsigned long long)t_ticks, t_ctx,
			   (unsigned long long)w_ticks, w_ctx);
}

static struct kobj_attribute stats_attr = __ATTR_RO(stats);

static struct attribute *tw_attrs[] = {
	&stats_attr.attr,
	NULL
};

static const struct attribute_group tw_attr_group = {
	.attrs = tw_attrs,
};

static struct kobject *tw_kobj;

static int __init timers_workqueues_init(void)
{
	int ret;

	tw_kobj = kobject_create_and_add("timers_workqueues", kernel_kobj);
	if (!tw_kobj)
		return -ENOMEM;

	ret = sysfs_create_group(tw_kobj, &tw_attr_group);
	if (ret) {
		kobject_put(tw_kobj);
		return ret;
	}

	timer_setup(&heartbeat_timer, heartbeat_timer_fn, 0);
	mod_timer(&heartbeat_timer,
		  jiffies + msecs_to_jiffies(timer_interval_ms));

	INIT_DELAYED_WORK(&heartbeat_work, heartbeat_work_fn);
	schedule_delayed_work(&heartbeat_work,
			      msecs_to_jiffies(work_interval_ms));

	pr_info("ready: /sys/kernel/timers_workqueues/stats timer=%ums work=%ums\n",
		timer_interval_ms, work_interval_ms);

	return 0;
}

static void __exit timers_workqueues_exit(void)
{
	timer_shutdown_sync(&heartbeat_timer);
	cancel_delayed_work_sync(&heartbeat_work);

	sysfs_remove_group(tw_kobj, &tw_attr_group);
	kobject_put(tw_kobj);

	pr_info("unloaded: timer_ticks=%llu work_ticks=%llu\n",
		(unsigned long long)timer_ticks,
		(unsigned long long)work_ticks);
}

module_init(timers_workqueues_init);
module_exit(timers_workqueues_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("guguali");
MODULE_DESCRIPTION("A timer (softirq context) and a workqueue (process context), compared live");
