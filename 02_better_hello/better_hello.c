/*  
 * better_hello.c 
 */

#include <linux/module.h>
#include <linux/init.h>

static int __init my_init(void)
{
    printk(KERN_INFO "Hello luv .\n");

    /*
     * A non-zero return means init_module failed;
     * module can't be loaded.
     */
    return 0;
}

static void __exit my_exit(void)
{
    printk(KERN_INFO "bye bye my luv\n");
}
module_init(my_init);
module_exit(my_exit); 


MODULE_LICENSE("GPL");
MODULE_AUTHOR("guguali");
MODULE_DESCRIPTION("A simple hello World Linux Kernel Module");