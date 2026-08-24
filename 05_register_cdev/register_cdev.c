#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/ktime.h>
#include <linux/sched.h>
#include <linux/atomic.h>

#define DEVICE_NAME "register_cdev"

/*
 * Dynamically assigned character-device major number.
 *
 * This variable lives in the loaded module's static memory
 * for the lifetime of the module.
 */
static int major;


/*
 * Time at which the module was loaded.
 */
static u64 loaded_at_ns;


/*
 * Number of currently open file descriptors referring
 * to our character device.
 */
static atomic_t open_count = ATOMIC_INIT(0);


/*
 * Called when userspace opens a device node whose
 * major/minor maps to this driver.
 *
 * Example:
 *
 *     open("/dev/register_cdev0", ...)
 *
 */
static int register_cdev_open(
    struct inode *inode,
    struct file *file
)
{
    int count;

    count = atomic_inc_return(&open_count);

    pr_info(
        DEVICE_NAME
        ": open: dev=%u:%u ctx=%s[%d] inode=%p file=%p opens=%d\n",
        imajor(inode),
        iminor(inode),
        current->comm,
        current->pid,
        inode,
        file,
        count
    );

    return 0;
}


/*
 * Called when userspace reads from our device.
 *
 * Example:
 *
 *     cat /dev/register_cdev0
 *
 *
 * buffer:
 *     userspace destination buffer
 *
 * count:
 *     maximum number of bytes userspace requested
 *
 * offset:
 *     current position inside the virtual device file
 */
static ssize_t register_cdev_read(
    struct file *file,
    char __user *buffer,
    size_t count,
    loff_t *offset
)
{
    char message[256];

    int message_length;
    size_t bytes_to_copy;

    unsigned int minor;


    /*
     * Determine which minor number was opened.
     *
     * file_inode(file) gives us the inode associated
     * with this open file.
     */
    minor = iminor(file_inode(file));


    /*
     * Construct data inside KERNEL MEMORY.
     *
     * "message" is a local variable and therefore
     * lives on the kernel stack of the currently
     * executing process.
     */
    message_length = scnprintf(
        message,
        sizeof(message),

        "register_cdev kernel device\n"
        "major=%d\n"
        "minor=%u\n"
        "context=%s[%d]\n",

        major,
        minor,
        current->comm,
        current->pid
    );


    /*
     * If userspace has already consumed the entire
     * message, return EOF.
     *
     * This is required so commands such as:
     *
     *     cat /dev/register_cdev0
     *
     * eventually stop reading.
     */
    if (*offset >= message_length)
        return 0;


    /*
     * Determine how many bytes we can copy.
     */
    bytes_to_copy = message_length - *offset;

    if (bytes_to_copy > count)
        bytes_to_copy = count;


    /*
     * Copy data from KERNEL MEMORY
     * into USERSPACE MEMORY.
     *
     * We must not directly dereference the userspace
     * pointer.
     */
    if (copy_to_user(
            buffer,
            message + *offset,
            bytes_to_copy
        )) {

        return -EFAULT;
    }


    /*
     * Advance the file position.
     */
    *offset += bytes_to_copy;


    pr_info(
        DEVICE_NAME
        ": read: dev=%d:%u bytes=%zu offset=%lld ctx=%s[%d]\n",
        major,
        minor,
        bytes_to_copy,
        *offset,
        current->comm,
        current->pid
    );


    return bytes_to_copy;
}


/*
 * Called when the userspace process closes its
 * file descriptor.
 */
static int register_cdev_release(
    struct inode *inode,
    struct file *file
)
{
    int count;

    count = atomic_dec_return(&open_count);

    pr_info(
        DEVICE_NAME
        ": release: dev=%u:%u ctx=%s[%d] opens=%d\n",
        imajor(inode),
        iminor(inode),
        current->comm,
        current->pid,
        count
    );

    return 0;
}


/*
 * VFS operation table.
 *
 * This structure connects generic Linux file operations
 * to functions inside our kernel module.
 */
static const struct file_operations register_cdev_fops = {

    /*
     * Prevent module removal while the device is being used.
     */
    .owner = THIS_MODULE,

    /*
     * open()
     */
    .open = register_cdev_open,

    /*
     * read()
     */
    .read = register_cdev_read,

    /*
     * close()
     */
    .release = register_cdev_release,
};


/*
 * Module initialization.
 *
 * Runs when:
 *
 *     sudo insmod register_cdev.ko
 */
static int __init register_cdev_init(void)
{
    u64 start_ns;
    u64 elapsed_ns;


    start_ns = ktime_get_ns();

    loaded_at_ns = start_ns;


    /*
     * major = 0 means:
     *
     *     dynamically allocate an available major number
     *
     * register_chrdev() registers this character device
     * with the kernel and associates it with our
     * file_operations structure.
     */
    major = register_chrdev(
        0,
        DEVICE_NAME,
        &register_cdev_fops
    );


    /*
     * Negative values represent Linux errno values.
     */
    if (major < 0) {

        pr_err(
            DEVICE_NAME
            ": init: register_chrdev failed err=%d\n",
            major
        );

        return major;
    }


    elapsed_ns = ktime_get_ns() - start_ns;


    pr_info(
        DEVICE_NAME
        ": init: major=%d minors=0-255 ctx=%s[%d] time=%llu us\n",
        major,
        current->comm,
        current->pid,
        (unsigned long long)(elapsed_ns / 1000ULL)
    );


    /*
     * Print addresses of objects belonging to this module.
     *
     * Modern kernels may hash/restrict pointer output.
     */
    pr_info(
        DEVICE_NAME
        ": objects: module=%p fops=%p major_var=%p open_count=%p\n",
        THIS_MODULE,
        &register_cdev_fops,
        &major,
        &open_count
    );


    return 0;
}


/*
 * Module cleanup.
 *
 * Runs when:
 *
 *     sudo rmmod register_cdev
 */
static void __exit register_cdev_exit(void)
{
    u64 start_ns;
    u64 uptime_ns;
    u64 elapsed_ns;


    start_ns = ktime_get_ns();

    uptime_ns = start_ns - loaded_at_ns;


    pr_info(
        DEVICE_NAME
        ": exit: major=%d opens=%d ctx=%s[%d] uptime=%llu ms\n",
        major,
        atomic_read(&open_count),
        current->comm,
        current->pid,
        (unsigned long long)(uptime_ns / 1000000ULL)
    );


    /*
     * Remove our character-device registration
     * from the kernel.
     */
    unregister_chrdev(
        major,
        DEVICE_NAME
    );


    elapsed_ns = ktime_get_ns() - start_ns;


    pr_info(
        DEVICE_NAME
        ": exit: unregistered major=%d time=%llu us\n",
        major,
        (unsigned long long)(elapsed_ns / 1000ULL)
    );
}


module_init(register_cdev_init);
module_exit(register_cdev_exit);


MODULE_LICENSE("GPL");
MODULE_AUTHOR("guguali");
MODULE_DESCRIPTION(
    "Character device registration, memory and read experiment"
);