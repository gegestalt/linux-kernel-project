// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/init.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/printk.h>

#define LOG_LEVEL_NONE	(-1)
#define LOG_LEVEL_MIN	0
#define LOG_LEVEL_MAX	7

/*
 * By default the module only prints normal lifecycle information.
 *
 * Set:
 *
 *	emit_all_levels=1
 *
 * to deliberately generate messages at all eight printk priorities.
 */
static bool emit_all_levels;
module_param(emit_all_levels, bool, 0444);
MODULE_PARM_DESC(emit_all_levels,
		 "Emit demonstration messages at all printk priorities");

/*
 * Select one individual printk priority.
 *
 *	-1 = disabled
 *	 0 = KERN_EMERG
 *	 1 = KERN_ALERT
 *	 2 = KERN_CRIT
 *	 3 = KERN_ERR
 *	 4 = KERN_WARNING
 *	 5 = KERN_NOTICE
 *	 6 = KERN_INFO
 *	 7 = KERN_DEBUG
 */
static int log_level = LOG_LEVEL_NONE;
module_param(log_level, int, 0444);
MODULE_PARM_DESC(log_level,
		 "Print one priority: -1=off, 0=emerg ... 7=debug");

/*
 * Emit one message at the requested printk priority.
 */
static void printk_emit_level(int level)
{
	switch (level) {
	case 0:
		pr_emerg("level 0: KERN_EMERG demonstration\n");
		break;
	case 1:
		pr_alert("level 1: KERN_ALERT demonstration\n");
		break;
	case 2:
		pr_crit("level 2: KERN_CRIT demonstration\n");
		break;
	case 3:
		pr_err("level 3: KERN_ERR demonstration\n");
		break;
	case 4:
		pr_warn("level 4: KERN_WARNING demonstration\n");
		break;
	case 5:
		pr_notice("level 5: KERN_NOTICE demonstration\n");
		break;
	case 6:
		pr_info("level 6: KERN_INFO demonstration\n");
		break;
	case 7:
		/*
		 * pr_debug() can depend on kernel configuration and
		 * dynamic-debug settings, so also emit an explicit
		 * KERN_DEBUG printk message for comparison.
		 */
		pr_debug("level 7: KERN_DEBUG through pr_debug()\n");
		printk(KERN_DEBUG
		       pr_fmt("level 7: explicit KERN_DEBUG printk\n"));
		break;
	default:
		pr_warn("invalid requested log level: %d\n", level);
		break;
	}
}

/*
 * Emit one demonstration message at every standard printk priority.
 */
static void printk_emit_all_levels(void)
{
	int level;

	for (level = LOG_LEVEL_MIN; level <= LOG_LEVEL_MAX; level++)
		printk_emit_level(level);
}

static int __init printk_log_levels_init(void)
{
	pr_info("module loaded: emit_all_levels=%d log_level=%d\n",
		emit_all_levels, log_level);

	if (log_level < LOG_LEVEL_NONE || log_level > LOG_LEVEL_MAX) {
		pr_err("log_level must be between -1 and 7\n");
		return -EINVAL;
	}

	if (emit_all_levels) {
		pr_notice("emitting all printk priority levels\n");
		printk_emit_all_levels();
	} else if (log_level != LOG_LEVEL_NONE) {
		pr_notice("emitting requested printk level %d\n", log_level);
		printk_emit_level(log_level);
	} else {
		pr_info("no demonstration level requested\n");
	}

	return 0;
}

static void __exit printk_log_levels_exit(void)
{
	pr_info("module unloaded\n");
}

module_init(printk_log_levels_init);
module_exit(printk_log_levels_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("guguali");
MODULE_DESCRIPTION("Linux kernel printk priority and log filtering experiment");
