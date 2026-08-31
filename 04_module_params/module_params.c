// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#define DEVICE_NAME "module_params_demo"

#define MAX_REPEAT 8
#define MAX_OUTPUT 512

/*
 * --------------------------------------------------------------------------
 * PARAMETERS
 * --------------------------------------------------------------------------
 *
 * Every module_param() below creates a matching file under:
 *
 *     /sys/module/module_params/parameters/
 *
 * The permission bits passed as the third argument are the sysfs file
 * mode for *that specific file*, not a module-wide setting:
 *
 *     0        no sysfs file is created at all (load-time only)
 *     0444     readable, not writable: fixed after insmod
 *     0644     readable and writable: root can change it while loaded
 *
 * A 0644 parameter has no "on change" callback by default - writing to
 * its sysfs file simply stores the new value directly into the C
 * variable. Code that wants to react live to a changed value (like
 * greet_show() below) has to *read the variable fresh* each time it is
 * used, rather than caching it once at init.
 */

/* charp: a dynamically-typed string parameter. Read-only after load. */
static char *greeting = "hello from module_params";
module_param(greeting, charp, 0444);
MODULE_PARM_DESC(greeting, "Message the demo device repeats back");

/*
 * uint, writable. Try changing this with the module loaded:
 *
 *   echo 3 | sudo tee /sys/module/module_params/parameters/repeat_count
 *
 * then read /dev/module_params_demo again - no reload required.
 */
static unsigned int repeat_count = 1;
module_param(repeat_count, uint, 0644);
MODULE_PARM_DESC(repeat_count, "Repeat count for the greeting (clamped to 8)");

/* bool, writable: toggles extra diagnostic output live. */
static bool verbose;
module_param(verbose, bool, 0644);
MODULE_PARM_DESC(verbose, "Include parameter diagnostics in device output");

/*
 * module_param_named() exposes a C variable under a *different* sysfs
 * name than its identifier in this file - useful when the natural C
 * name would collide, or when you want a friendlier public name.
 */
static int dbg_level = 3;
module_param_named(log_level, dbg_level, int, 0444);
MODULE_PARM_DESC(log_level, "Read-only int (0-7), exposed under a renamed sysfs file");

/*
 * module_param_array(): a fixed-size array parameter, set as a
 * comma-separated list at load time, e.g.:
 *
 *   insmod module_params.ko primes=2,3,5,7
 *
 * `primes_count` receives how many elements were actually supplied.
 */
static int primes[4] = { 2, 3, 5, 7 };
static int primes_count;
module_param_array(primes, int, &primes_count, 0444);
MODULE_PARM_DESC(primes, "Up to 4 comma-separated ints, load-time only");

/*
 * --------------------------------------------------------------------------
 * DEMO DEVICE
 * --------------------------------------------------------------------------
 *
 * Every read() recomputes its output from the *current* parameter
 * values, so 0644 parameters (repeat_count, verbose) can be changed
 * through sysfs and observed here without unloading the module.
 */

static ssize_t module_params_read(struct file *file, char __user *buf,
				  size_t count, loff_t *ppos)
{
	char kbuf[MAX_OUTPUT];
	unsigned int reps;
	int len = 0;
	unsigned int i;

	reps = repeat_count;
	if (reps > MAX_REPEAT)
		reps = MAX_REPEAT;

	for (i = 0; i < reps && len < MAX_OUTPUT - 1; i++)
		len += scnprintf(kbuf + len, MAX_OUTPUT - len, "%s\n",
				  greeting);

	if (verbose) {
		len += scnprintf(kbuf + len, MAX_OUTPUT - len,
				  "-- diagnostics --\n"
				  "repeat_count(raw)=%u (clamped=%u)\n"
				  "log_level=%d\n"
				  "primes_count=%d primes[0]=%d\n",
				  repeat_count, reps, dbg_level,
				  primes_count, primes[0]);
	}

	if (*ppos >= len)
		return 0;

	if (count > (size_t)(len - *ppos))
		count = len - *ppos;

	if (copy_to_user(buf, kbuf + *ppos, count))
		return -EFAULT;

	*ppos += count;

	return count;
}

static const struct file_operations module_params_fops = {
	.owner = THIS_MODULE,
	.read = module_params_read,
};

static struct miscdevice module_params_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = DEVICE_NAME,
	.fops = &module_params_fops,
	.mode = 0444,
};

static int __init module_params_init(void)
{
	char primes_buf[32];
	int primes_len = 0;
	int ret;
	int i;

	pr_info("loaded: greeting=\"%s\" repeat_count=%u verbose=%d log_level=%d\n",
		greeting, repeat_count, verbose, dbg_level);

	for (i = 0; i < primes_count; i++)
		primes_len += scnprintf(primes_buf + primes_len,
					 sizeof(primes_buf) - primes_len,
					 "%d%s", primes[i],
					 i + 1 < primes_count ? "," : "");
	pr_info("primes: count=%d values=[%s]\n", primes_count, primes_buf);

	ret = misc_register(&module_params_miscdev);
	if (ret) {
		pr_err("misc_register failed: %d\n", ret);
		return ret;
	}

	pr_info("/dev/%s ready - try: cat /dev/%s\n", DEVICE_NAME, DEVICE_NAME);
	pr_info("live parameters: /sys/module/%s/parameters/\n", KBUILD_MODNAME);

	return 0;
}

static void __exit module_params_exit(void)
{
	misc_deregister(&module_params_miscdev);
	pr_info("unloaded\n");
}

module_init(module_params_init);
module_exit(module_params_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("guguali");
MODULE_DESCRIPTION("module_param() types, permissions and live-tunable state");
