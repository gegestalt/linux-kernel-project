// SPDX-License-Identifier: GPL-2.0-only

/*
 * Module 04 covers one example of each basic module_param*() flavor.
 * This module covers what real drivers actually do with parameters
 * once they're more than a passive settings box: a token-bucket rate
 * limiter, the same mechanism behind traffic shaping (tc's sch_tbf)
 * and countless "don't let this fire more than N times a second"
 * guards in the kernel.
 *
 * Every parameter here uses module_param_cb() with a custom
 * kernel_param_ops, not the plain module_param() macro, because every
 * one of them needs to do something module_param() alone can't:
 *
 *   - mode            validated against a fixed set of strings,
 *                      rejects anything else with -EINVAL
 *   - capacity         a live write immediately clamps the current
 *                      token count if it shrank below it
 *   - refill_rate      its .set validates against capacity's CURRENT
 *                      value - the two parameters are not independent
 *   - tokens_available read-only (0444), computed live under the same
 *                      lock the timer and consume path use - never
 *                      settable, only ever a snapshot
 *
 * label uses module_param_string() instead of charp (04's flavor) -
 * a fixed-size buffer the kernel owns outright, not a pointer to
 * something insmod/modprobe allocated for you.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/moduleparam.h>
#include <linux/spinlock.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/minmax.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>

#define LABEL_LEN 32

/* ---------------------------------------------------------------------
 * Protected state. Three things touch this: parameter .set callbacks
 * (sysfs write, any CPU), the refill timer (softirq context, HZ), and
 * the consume path (misc device write, any CPU). All three take
 * bucket_lock - this is not a decoration, module 11 covers what
 * happens to state exactly like this without it.
 * ---------------------------------------------------------------------
 */
static DEFINE_SPINLOCK(bucket_lock);
static int tokens;
static unsigned long total_consumed;
static unsigned long total_rejected;

/* ---------------------------------------------------------------------
 * mode: off | monitor | enforce
 * ---------------------------------------------------------------------
 */
enum limiter_mode { MODE_OFF, MODE_MONITOR, MODE_ENFORCE };
static enum limiter_mode mode = MODE_MONITOR;
static char mode_str[16] = "monitor";

static int mode_set(const char *val, const struct kernel_param *kp)
{
	const char *canonical;

	if (sysfs_streq(val, "off")) {
		mode = MODE_OFF;
		canonical = "off";
	} else if (sysfs_streq(val, "monitor")) {
		mode = MODE_MONITOR;
		canonical = "monitor";
	} else if (sysfs_streq(val, "enforce")) {
		mode = MODE_ENFORCE;
		canonical = "enforce";
	} else {
		return -EINVAL;
	}

	/*
	 * Store the canonical spelling, not val verbatim: sysfs_streq()
	 * already ignores a trailing '\n' when matching (every `echo`
	 * write carries one), but a plain strscpy(mode_str, val, ...)
	 * here would still copy that newline into mode_str - and then
	 * mode_get()'s own "%s\n" doubles it on readback. Confirmed live:
	 * `echo enforce > .../mode; od -c .../mode` read back
	 * "enforce\n\n", two newlines, before this fix.
	 */
	strscpy(mode_str, canonical, sizeof(mode_str));
	return 0;
}

static int mode_get(char *buffer, const struct kernel_param *kp)
{
	return sysfs_emit(buffer, "%s\n", mode_str);
}

static const struct kernel_param_ops mode_ops = {
	.set = mode_set,
	.get = mode_get,
};
module_param_cb(mode, &mode_ops, NULL, 0644);
MODULE_PARM_DESC(mode, "off | monitor | enforce - enforce rejects consume() at zero tokens");

/* ---------------------------------------------------------------------
 * capacity: bucket ceiling. Shrinking it live-clamps the current
 * token count under lock - a real side effect, not just storage.
 * ---------------------------------------------------------------------
 */
static int capacity = 100;

static int capacity_set(const char *val, const struct kernel_param *kp)
{
	int new_cap, ret;
	unsigned long flags;

	ret = kstrtoint(val, 0, &new_cap);
	if (ret)
		return ret;
	if (new_cap <= 0)
		return -EINVAL;

	spin_lock_irqsave(&bucket_lock, flags);
	capacity = new_cap;
	if (tokens > capacity)
		tokens = capacity;
	spin_unlock_irqrestore(&bucket_lock, flags);

	return 0;
}

static const struct kernel_param_ops capacity_ops = {
	.set = capacity_set,
	.get = param_get_int,
};
module_param_cb(capacity, &capacity_ops, &capacity, 0644);
MODULE_PARM_DESC(capacity, "maximum tokens the bucket can hold");

/* ---------------------------------------------------------------------
 * refill_rate: tokens added per tick. Must not exceed capacity - the
 * two parameters are validated against each other, not in isolation.
 * ---------------------------------------------------------------------
 */
static int refill_rate = 5;

static int refill_rate_set(const char *val, const struct kernel_param *kp)
{
	int new_rate, ret;
	unsigned long flags;
	bool reject;

	ret = kstrtoint(val, 0, &new_rate);
	if (ret)
		return ret;
	if (new_rate <= 0)
		return -EINVAL;

	spin_lock_irqsave(&bucket_lock, flags);
	reject = new_rate > capacity;
	if (!reject)
		refill_rate = new_rate;
	spin_unlock_irqrestore(&bucket_lock, flags);

	return reject ? -EINVAL : 0;
}

static const struct kernel_param_ops refill_rate_ops = {
	.set = refill_rate_set,
	.get = param_get_int,
};
module_param_cb(refill_rate, &refill_rate_ops, &refill_rate, 0644);
MODULE_PARM_DESC(refill_rate, "tokens added per tick - rejected if greater than capacity");

/* ---------------------------------------------------------------------
 * tokens_available: read-only (0444), computed live under lock. No
 * .set at all - not "settable but denied", genuinely not a thing you
 * can write, because it isn't independent state to begin with.
 * ---------------------------------------------------------------------
 */
static int tokens_get(char *buffer, const struct kernel_param *kp)
{
	int cur;
	unsigned long flags;

	spin_lock_irqsave(&bucket_lock, flags);
	cur = tokens;
	spin_unlock_irqrestore(&bucket_lock, flags);

	return sysfs_emit(buffer, "%d\n", cur);
}

static const struct kernel_param_ops tokens_ops = {
	.get = tokens_get,
};
module_param_cb(tokens_available, &tokens_ops, NULL, 0444);
MODULE_PARM_DESC(tokens_available, "current token count (read-only, computed live)");

/* ---------------------------------------------------------------------
 * label: module_param_string - a fixed LABEL_LEN buffer the kernel
 * owns directly, unlike 04's charp (a pointer insmod/modprobe target
 * points at a string it manages). Compare `readelf -x .data
 * token_bucket.ko` against 04's charp default: this one's bytes are
 * sitting in .data itself, not just a pointer to somewhere else.
 * ---------------------------------------------------------------------
 */
static char label[LABEL_LEN] = "unnamed-limiter";
module_param_string(label, label, LABEL_LEN, 0644);
MODULE_PARM_DESC(label, "human-readable name for this limiter instance");

/* ---------------------------------------------------------------------
 * Periodic refill - matches module 14's timer_setup()/mod_timer()
 * pattern exactly.
 * ---------------------------------------------------------------------
 */
static struct timer_list refill_timer;

static void refill_fn(struct timer_list *t)
{
	unsigned long flags;

	spin_lock_irqsave(&bucket_lock, flags);
	tokens = min(tokens + refill_rate, capacity);
	spin_unlock_irqrestore(&bucket_lock, flags);

	mod_timer(&refill_timer, jiffies + HZ);
}

/* ---------------------------------------------------------------------
 * consume: a write() to /dev/token_bucket_consume spends one token.
 * In MODE_ENFORCE, spending against an empty bucket fails with
 * -EBUSY; in MODE_OFF/MODE_MONITOR it's counted but always allowed -
 * the same distinction real rate limiters draw between "log it" and
 * "actually block it".
 * ---------------------------------------------------------------------
 */
static ssize_t consume_write(struct file *f, const char __user *buf,
			     size_t len, loff_t *off)
{
	unsigned long flags;
	bool had_token;

	spin_lock_irqsave(&bucket_lock, flags);
	had_token = tokens > 0;
	if (had_token) {
		tokens--;
		total_consumed++;
	} else {
		total_rejected++;
	}
	spin_unlock_irqrestore(&bucket_lock, flags);

	if (!had_token && mode == MODE_ENFORCE)
		return -EBUSY;

	return len;
}

static const struct file_operations consume_fops = {
	.owner = THIS_MODULE,
	.write = consume_write,
};

static struct miscdevice consume_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = "token_bucket_consume",
	.fops  = &consume_fops,
};

static int __init token_bucket_init(void)
{
	int ret;

	ret = misc_register(&consume_dev);
	if (ret)
		return ret;

	tokens = capacity;
	timer_setup(&refill_timer, refill_fn, 0);
	mod_timer(&refill_timer, jiffies + HZ);

	pr_info("token_bucket: '%s' up - capacity=%d refill_rate=%d mode=%s\n",
		label, capacity, refill_rate, mode_str);
	return 0;
}

static void __exit token_bucket_exit(void)
{
	/* not timer_delete_sync(): this timer is never coming back, and
	 * shutdown additionally poisons it against a racing mod_timer()
	 * from refill_fn() itself - the right call specifically for an
	 * unload path.
	 */
	timer_shutdown_sync(&refill_timer);
	misc_deregister(&consume_dev);
	pr_info("token_bucket: '%s' down - consumed=%lu rejected=%lu\n",
		label, total_consumed, total_rejected);
}

module_init(token_bucket_init);
module_exit(token_bucket_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("guguali");
MODULE_DESCRIPTION("Advanced module parameters: a token-bucket rate limiter with validated, interdependent, live-effecting parameters");
