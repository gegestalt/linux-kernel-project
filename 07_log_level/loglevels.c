// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/init.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/printk.h>

static bool emit_all_levels = true;
module_param(emit_all_levels, bool, 0644);
MODULE_PARM_DESC(emit_all_levels,
		 "Emit demonstration messages for all kernel log levels");

/*
 * Demonstrate the standard Linux kernel printk priorities.
 *
 * These deliberately alarming messages are only examples for this
 * tutorial. They do not indicate actual kernel failures.
 */
static void printk_log_levels_demo(void)
{
	pr_emerg("demo level 0: KERN_EMERG\n");
	pr_alert("demo level 1: KERN_ALERT\n");
	pr_crit("demo level 2: KERN_CRIT\n");
	pr_err("demo level 3: KERN_ERR\n");
	pr_warn("demo level 4: KERN_WARNING\n");
	pr_notice("demo level 5: KERN_NOTICE\n");
	pr_info("demo level 6: KERN_INFO\n");

	/*
	 * pr_debug() may be compiled out or controlled through
	 * dynamic debug depending on the kernel configuration.
	 */
	pr_debug("demo level 7: KERN_DEBUG via pr_debug\n");

	/*
	 * Use printk(KERN_DEBUG ...) as an explicit level-7 example.
	 * Whether it is displayed on the console still depends on
	 * the current console log level.
	 */
	printk(KERN_DEBUG pr_fmt("demo level 7: explicit KERN_DEBUG\n"));
}

static int __init printk_log_levels_init(void)
{
	pr_info("module loaded\n");

	if (emit_all_levels)
		printk_log_levels_demo();
	else
		pr_info("log-level demonstration disabled\n");

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
MODULE_DESCRIPTION("Linux kernel printk log-level experiment");