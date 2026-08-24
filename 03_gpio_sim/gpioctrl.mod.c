#include <linux/module.h>
#include <linux/export-internal.h>
#include <linux/compiler.h>

MODULE_INFO(name, KBUILD_MODNAME);

__visible struct module __this_module
__section(".gnu.linkonce.this_module") = {
	.name = KBUILD_MODNAME,
	.init = init_module,
#ifdef CONFIG_MODULE_UNLOAD
	.exit = cleanup_module,
#endif
	.arch = MODULE_ARCH_INIT,
};



static const struct modversion_info ____versions[]
__used __section("__versions") = {
	{ 0xe8213e80, "_printk" },
	{ 0x9aa6980d, "mutex_lock" },
	{ 0x9aa6980d, "mutex_unlock" },
	{ 0xdd6830c7, "sysfs_emit" },
	{ 0x8e142c2e, "kstrtouint" },
	{ 0xd272d446, "__stack_chk_fail" },
	{ 0xaef1f20d, "system_wq" },
	{ 0x534ed5f3, "__msecs_to_jiffies" },
	{ 0x8ce83585, "mod_delayed_work_on" },
	{ 0x97acb853, "ktime_get" },
	{ 0x85acaba2, "cancel_delayed_work_sync" },
	{ 0x5d6118e9, "sysfs_remove_group" },
	{ 0x3c568d08, "misc_deregister" },
	{ 0x24631bc5, "gpiod_get_value_cansleep" },
	{ 0x631668c1, "gpiod_set_value_cansleep" },
	{ 0x616e9b7a, "gpio_device_put" },
	{ 0xcb8b6ec6, "kfree" },
	{ 0xe4de56b4, "__ubsan_handle_load_invalid_value" },
	{ 0x30c65558, "strnlen" },
	{ 0xd70733be, "sized_strscpy" },
	{ 0xe54e0a6b, "__fortify_panic" },
	{ 0x5373d78a, "kstrtobool" },
	{ 0x0e9cab28, "memset" },
	{ 0x40a621c5, "scnprintf" },
	{ 0x437e81c7, "simple_read_from_buffer" },
	{ 0xbd03ed67, "random_kmalloc_seed" },
	{ 0xdeb42f11, "kmalloc_caches" },
	{ 0xef2d97e9, "__kmalloc_cache_noprof" },
	{ 0x9aa6980d, "mutex_init_generic" },
	{ 0xc3336a36, "gpio_device_find_by_label" },
	{ 0xe8858688, "gpio_device_get_desc" },
	{ 0x7b39a9f8, "gpiod_direction_input" },
	{ 0x631668c1, "gpiod_direction_output" },
	{ 0x71798f7e, "delayed_work_timer_fn" },
	{ 0x02f9bbf0, "timer_init_key" },
	{ 0xf5a4e43d, "misc_register" },
	{ 0xc8b982a1, "sysfs_create_group" },
	{ 0xaef1f20d, "system_percpu_wq" },
	{ 0x8ce83585, "queue_delayed_work_on" },
	{ 0x0e675b65, "___ratelimit" },
	{ 0x5cb46e6d, "validate_usercopy_range" },
	{ 0xa61fd7aa, "__check_object_size" },
	{ 0xaa47b76e, "__arch_copy_from_user" },
	{ 0x41495f0d, "strim" },
	{ 0xdd45951a, "sysfs_streq" },
	{ 0x7c1c5981, "strncmp" },
	{ 0x9c4ed43a, "alt_cb_patch_nops" },
	{ 0x90a48d82, "__ubsan_handle_out_of_bounds" },
	{ 0x2aca9b49, "param_ops_bool" },
	{ 0x2aca9b49, "param_ops_uint" },
	{ 0x2aca9b49, "param_ops_int" },
	{ 0x2aca9b49, "param_ops_charp" },
	{ 0xc7ea9460, "module_layout" },
};

static const u32 ____version_ext_crcs[]
__used __section("__version_ext_crcs") = {
	0xe8213e80,
	0x9aa6980d,
	0x9aa6980d,
	0xdd6830c7,
	0x8e142c2e,
	0xd272d446,
	0xaef1f20d,
	0x534ed5f3,
	0x8ce83585,
	0x97acb853,
	0x85acaba2,
	0x5d6118e9,
	0x3c568d08,
	0x24631bc5,
	0x631668c1,
	0x616e9b7a,
	0xcb8b6ec6,
	0xe4de56b4,
	0x30c65558,
	0xd70733be,
	0xe54e0a6b,
	0x5373d78a,
	0x0e9cab28,
	0x40a621c5,
	0x437e81c7,
	0xbd03ed67,
	0xdeb42f11,
	0xef2d97e9,
	0x9aa6980d,
	0xc3336a36,
	0xe8858688,
	0x7b39a9f8,
	0x631668c1,
	0x71798f7e,
	0x02f9bbf0,
	0xf5a4e43d,
	0xc8b982a1,
	0xaef1f20d,
	0x8ce83585,
	0x0e675b65,
	0x5cb46e6d,
	0xa61fd7aa,
	0xaa47b76e,
	0x41495f0d,
	0xdd45951a,
	0x7c1c5981,
	0x9c4ed43a,
	0x90a48d82,
	0x2aca9b49,
	0x2aca9b49,
	0x2aca9b49,
	0x2aca9b49,
	0xc7ea9460,
};
static const char ____version_ext_names[]
__used __section("__version_ext_names") =
	"_printk\0"
	"mutex_lock\0"
	"mutex_unlock\0"
	"sysfs_emit\0"
	"kstrtouint\0"
	"__stack_chk_fail\0"
	"system_wq\0"
	"__msecs_to_jiffies\0"
	"mod_delayed_work_on\0"
	"ktime_get\0"
	"cancel_delayed_work_sync\0"
	"sysfs_remove_group\0"
	"misc_deregister\0"
	"gpiod_get_value_cansleep\0"
	"gpiod_set_value_cansleep\0"
	"gpio_device_put\0"
	"kfree\0"
	"__ubsan_handle_load_invalid_value\0"
	"strnlen\0"
	"sized_strscpy\0"
	"__fortify_panic\0"
	"kstrtobool\0"
	"memset\0"
	"scnprintf\0"
	"simple_read_from_buffer\0"
	"random_kmalloc_seed\0"
	"kmalloc_caches\0"
	"__kmalloc_cache_noprof\0"
	"mutex_init_generic\0"
	"gpio_device_find_by_label\0"
	"gpio_device_get_desc\0"
	"gpiod_direction_input\0"
	"gpiod_direction_output\0"
	"delayed_work_timer_fn\0"
	"timer_init_key\0"
	"misc_register\0"
	"sysfs_create_group\0"
	"system_percpu_wq\0"
	"queue_delayed_work_on\0"
	"___ratelimit\0"
	"validate_usercopy_range\0"
	"__check_object_size\0"
	"__arch_copy_from_user\0"
	"strim\0"
	"sysfs_streq\0"
	"strncmp\0"
	"alt_cb_patch_nops\0"
	"__ubsan_handle_out_of_bounds\0"
	"param_ops_bool\0"
	"param_ops_uint\0"
	"param_ops_int\0"
	"param_ops_charp\0"
	"module_layout\0"
;

MODULE_INFO(depends, "");


MODULE_INFO(srcversion, "1D95C6DA4A585D9A0E170A7");
