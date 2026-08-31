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
	{ 0x30eb81ed, "__dynamic_pr_debug" },
	{ 0xd272d446, "dump_stack" },
	{ 0xc7ea9460, "module_layout" },
};

static const u32 ____version_ext_crcs[]
__used __section("__version_ext_crcs") = {
	0xe8213e80,
	0x30eb81ed,
	0xd272d446,
	0xc7ea9460,
};
static const char ____version_ext_names[]
__used __section("__version_ext_names") =
	"_printk\0"
	"__dynamic_pr_debug\0"
	"dump_stack\0"
	"module_layout\0"
;

MODULE_INFO(depends, "");


MODULE_INFO(srcversion, "90D3F8D3479B1C3045225CC");
