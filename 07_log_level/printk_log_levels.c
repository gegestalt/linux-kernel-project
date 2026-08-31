// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/init.h>
#include <linux/module.h>
#include <linux/printk.h>

/*
 * Emit one message for every standard printk priority.
 *
 * The pr_*() helpers already provide their corresponding KERN_* priority,
 * so an explicit KERN_* prefix is not required for those calls.
 */
static void printk_emit_all_levels(void)
{
	pr_emerg("level 0: KERN_EMERG demonstration\n");
	pr_alert("level 1: KERN_ALERT demonstration\n");
	pr_crit("level 2: KERN_CRIT demonstration\n");
	pr_err("level 3: KERN_ERR demonstration\n");
	pr_warn("level 4: KERN_WARNING demonstration\n");
	pr_notice("level 5: KERN_NOTICE demonstration\n");
	pr_info("level 6: KERN_INFO demonstration\n");

	/*
	 * pr_debug() output can depend on the kernel configuration and
	 * dynamic-debug settings.
	 */
	pr_debug("level 7: KERN_DEBUG through pr_debug()\n");

	/*
	 * Keep an explicit printk(KERN_DEBUG ...) call so the experiment
	 * can compare raw printk output with the pr_debug() helper.
	 */
	printk(KERN_DEBUG
	       pr_fmt("level 7: KERN_DEBUG through explicit printk()\n"));
}

static int __init printk_log_levels_init(void)
{
	pr_info("module loaded - emitting all printk priorities\n");

	printk_emit_all_levels();

	pr_info("all printk priority demonstrations emitted\n");

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
