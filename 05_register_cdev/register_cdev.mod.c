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
	{ 0x9c4ed43a, "alt_cb_patch_nops" },
	{ 0xe8213e80, "_printk" },
	{ 0x97acb853, "ktime_get" },
	{ 0x9f40457e, "__register_chrdev" },
	{ 0x52b15b3b, "__unregister_chrdev" },
	{ 0x0e9cab28, "memset" },
	{ 0x40a621c5, "scnprintf" },
	{ 0x5cb46e6d, "validate_usercopy_range" },
	{ 0xa61fd7aa, "__check_object_size" },
	{ 0xaa47b76e, "__arch_copy_to_user" },
	{ 0xd272d446, "__stack_chk_fail" },
	{ 0xf64ac983, "__copy_overflow" },
	{ 0xc7ea9460, "module_layout" },
};

static const u32 ____version_ext_crcs[]
__used __section("__version_ext_crcs") = {
	0x9c4ed43a,
	0xe8213e80,
	0x97acb853,
	0x9f40457e,
	0x52b15b3b,
	0x0e9cab28,
	0x40a621c5,
	0x5cb46e6d,
	0xa61fd7aa,
	0xaa47b76e,
	0xd272d446,
	0xf64ac983,
	0xc7ea9460,
};
static const char ____version_ext_names[]
__used __section("__version_ext_names") =
	"alt_cb_patch_nops\0"
	"_printk\0"
	"ktime_get\0"
	"__register_chrdev\0"
	"__unregister_chrdev\0"
	"memset\0"
	"scnprintf\0"
	"validate_usercopy_range\0"
	"__check_object_size\0"
	"__arch_copy_to_user\0"
	"__stack_chk_fail\0"
	"__copy_overflow\0"
	"module_layout\0"
;

MODULE_INFO(depends, "");


MODULE_INFO(srcversion, "463999C344EE44CC52BB753");
