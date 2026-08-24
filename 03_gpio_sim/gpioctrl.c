/*
 * gpioctrl.c
 *
 * Educational GPIO driver for gpio-sim.
 *
 * Demonstrates:
 *   - Linux module lifecycle
 *   - GPIO descriptors
 *   - delayed work / workqueues
 *   - dynamic kernel memory
 *   - mutex-protected shared state
 *   - sysfs
 *   - misc character device
 *   - module parameters
 *   - execution contexts
 *   - timing / runtime statistics
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/errno.h>

/* Timing / deferred execution */
#include <linux/ktime.h>
#include <linux/workqueue.h>
#include <linux/jiffies.h>

/* Dynamic kernel memory + synchronization */
#include <linux/slab.h>
#include <linux/mutex.h>

/* Character device / userspace access */
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>

/* sysfs */
#include <linux/device.h>
#include <linux/sysfs.h>

/* Process/execution-context information */
#include <linux/sched.h>

/* String parsing */
#include <linux/string.h>

/* GPIO */
#include <linux/gpio/consumer.h>
#include <linux/gpio/driver.h>


#define DRIVER_NAME "gpioctrl"

#define MIN_POLL_MS 10
#define MAX_POLL_MS 60000


/*
 * --------------------------------------------------------------------------
 * LOAD-TIME MODULE PARAMETERS
 * --------------------------------------------------------------------------
 *
 * These become visible under:
 *
 * /sys/module/gpioctrl/parameters/
 *
 * They are read-only after loading (0444).
 */

static char *gpio_label = "gpio-sim.0:node0";
module_param(gpio_label, charp, 0444);
MODULE_PARM_DESC(gpio_label, "GPIO controller label");


static int button_offset = 20;
module_param(button_offset, int, 0444);
MODULE_PARM_DESC(button_offset, "Button GPIO offset");


static int led_offset = 21;
module_param(led_offset, int, 0444);
MODULE_PARM_DESC(led_offset, "LED GPIO offset");


static unsigned int initial_poll_ms = 500;
module_param(initial_poll_ms, uint, 0444);
MODULE_PARM_DESC(initial_poll_ms, "Initial polling interval in milliseconds");


static bool initial_invert;
module_param(initial_invert, bool, 0444);
MODULE_PARM_DESC(initial_invert, "Initial LED inversion state");


/*
 * --------------------------------------------------------------------------
 * DYNAMIC DRIVER STATE
 * --------------------------------------------------------------------------
 *
 * The POINTER:
 *
 *     state
 *
 * is a global variable belonging to the module.
 *
 * The actual struct is allocated using:
 *
 *     kzalloc()
 *
 * and therefore lives in dynamically allocated kernel/slab memory.
 *
 *
 * MODULE MEMORY                     KERNEL SLAB MEMORY
 *
 * +-----------------+              +-------------------------+
 * | state pointer   | -----------> | struct gpioctrl_state   |
 * +-----------------+              |                         |
 *                                  | mutex                   |
 *                                  | button state            |
 *                                  | LED state               |
 *                                  | counters                |
 *                                  | timing                  |
 *                                  | execution context       |
 *                                  +-------------------------+
 */

struct gpioctrl_state {
    struct mutex lock;

    int button;
    int led;

    bool invert;
    unsigned int poll_ms;

    u64 samples;
    u64 changes;
    u64 output_updates;

    u64 loaded_at_ns;

    pid_t init_pid;
    char init_comm[TASK_COMM_LEN];

    pid_t last_sample_pid;
    char last_sample_comm[TASK_COMM_LEN];
};


static struct gpioctrl_state *state;


/*
 * --------------------------------------------------------------------------
 * GPIO OBJECT REFERENCES
 * --------------------------------------------------------------------------
 *
 * These pointers do NOT contain the GPIO objects themselves.
 *
 * They point to objects managed by the GPIO subsystem.
 */

static struct gpio_device *gdev;
static struct gpio_desc *button;
static struct gpio_desc *led;


/*
 * --------------------------------------------------------------------------
 * DELAYED WORK
 * --------------------------------------------------------------------------
 *
 * gpio_work itself is part of module-global memory.
 *
 * When scheduled, gpioctrl_work_fn() normally executes from a
 * kernel worker thread such as:
 *
 *     kworker/0:2
 *
 * rather than from insmod/rmmod.
 */

static struct delayed_work gpio_work;


/*
 * Return the current polling interval while protecting shared state.
 */
static unsigned int gpioctrl_get_poll_ms(void)
{
    unsigned int value;

    mutex_lock(&state->lock);
    value = state->poll_ms;
    mutex_unlock(&state->lock);

    return value;
}


/*
 * Read the button and synchronize LED state.
 *
 * This can execute from:
 *
 *   - a kworker
 *   - a sysfs write callback
 *   - a /dev/gpioctrl write()
 *
 * All of those are process contexts where *_cansleep GPIO APIs
 * are appropriate.
 */
static int gpioctrl_sample_once(bool force_log)
{
    int button_value;
    int desired_led;

    bool changed;

    u64 samples;
    u64 changes;
    u64 updates;

    int final_led;

    button_value = gpiod_get_value_cansleep(button);

    if (button_value < 0)
        return button_value;


    mutex_lock(&state->lock);

    desired_led = state->invert ?
        !button_value :
        button_value;


    changed =
        (state->button != -1) &&
        (button_value != state->button);


    state->samples++;


    if (changed)
        state->changes++;


    /*
     * Only touch the output when required.
     */
    if (desired_led != state->led) {

        gpiod_set_value_cansleep(
            led,
            desired_led
        );

        state->led = desired_led;
        state->output_updates++;
    }


    state->button = button_value;


    /*
     * Record execution context.
     */
    state->last_sample_pid = current->pid;

    strscpy(
        state->last_sample_comm,
        current->comm,
        TASK_COMM_LEN
    );


    /*
     * Snapshot values while protected by the mutex.
     */
    samples = state->samples;
    changes = state->changes;
    updates = state->output_updates;

    final_led = state->led;

    mutex_unlock(&state->lock);


    if (changed || force_log) {
        pr_info(
            DRIVER_NAME
            ": state: button=%d led=%d samples=%llu "
            "changes=%llu updates=%llu ctx=%s[%d]\n",

            button_value,
            final_led,

            (unsigned long long)samples,
            (unsigned long long)changes,
            (unsigned long long)updates,

            current->comm,
            current->pid
        );
    }


    return 0;
}


/*
 * --------------------------------------------------------------------------
 * WORKQUEUE CALLBACK
 * --------------------------------------------------------------------------
 */

static void gpioctrl_work_fn(struct work_struct *work)
{
    unsigned int delay;
    int ret;

    ret = gpioctrl_sample_once(false);

    if (ret < 0) {
        pr_err_ratelimited(
            DRIVER_NAME
            ": poll: GPIO read failed err=%d\n",
            ret
        );
    }


    delay = gpioctrl_get_poll_ms();


    schedule_delayed_work(
        &gpio_work,
        msecs_to_jiffies(delay)
    );
}


/*
 * --------------------------------------------------------------------------
 * SYSFS
 * --------------------------------------------------------------------------
 *
 * These files will appear under:
 *
 * /sys/class/misc/gpioctrl/
 *
 * Example:
 *
 * button
 * led
 * invert
 * poll_ms
 * samples
 * changes
 * output_updates
 * uptime_ms
 */


/* button */

static ssize_t button_show(
    struct device *dev,
    struct device_attribute *attr,
    char *buf)
{
    int value;

    mutex_lock(&state->lock);
    value = state->button;
    mutex_unlock(&state->lock);

    return sysfs_emit(buf, "%d\n", value);
}

static DEVICE_ATTR_RO(button);


/* led */

static ssize_t led_show(
    struct device *dev,
    struct device_attribute *attr,
    char *buf)
{
    int value;

    mutex_lock(&state->lock);
    value = state->led;
    mutex_unlock(&state->lock);

    return sysfs_emit(buf, "%d\n", value);
}

static DEVICE_ATTR_RO(led);


/* samples */

static ssize_t samples_show(
    struct device *dev,
    struct device_attribute *attr,
    char *buf)
{
    u64 value;

    mutex_lock(&state->lock);
    value = state->samples;
    mutex_unlock(&state->lock);

    return sysfs_emit(
        buf,
        "%llu\n",
        (unsigned long long)value
    );
}

static DEVICE_ATTR_RO(samples);


/* changes */

static ssize_t changes_show(
    struct device *dev,
    struct device_attribute *attr,
    char *buf)
{
    u64 value;

    mutex_lock(&state->lock);
    value = state->changes;
    mutex_unlock(&state->lock);

    return sysfs_emit(
        buf,
        "%llu\n",
        (unsigned long long)value
    );
}

static DEVICE_ATTR_RO(changes);


/* output_updates */

static ssize_t output_updates_show(
    struct device *dev,
    struct device_attribute *attr,
    char *buf)
{
    u64 value;

    mutex_lock(&state->lock);
    value = state->output_updates;
    mutex_unlock(&state->lock);

    return sysfs_emit(
        buf,
        "%llu\n",
        (unsigned long long)value
    );
}

static DEVICE_ATTR_RO(output_updates);


/* uptime_ms */

static ssize_t uptime_ms_show(
    struct device *dev,
    struct device_attribute *attr,
    char *buf)
{
    u64 loaded_at;
    u64 uptime;

    mutex_lock(&state->lock);
    loaded_at = state->loaded_at_ns;
    mutex_unlock(&state->lock);

    uptime =
        ktime_get_ns() -
        loaded_at;

    return sysfs_emit(
        buf,
        "%llu\n",
        (unsigned long long)(
            uptime / 1000000ULL
        )
    );
}

static DEVICE_ATTR_RO(uptime_ms);


/* invert */

static ssize_t invert_show(
    struct device *dev,
    struct device_attribute *attr,
    char *buf)
{
    bool value;

    mutex_lock(&state->lock);
    value = state->invert;
    mutex_unlock(&state->lock);

    return sysfs_emit(
        buf,
        "%d\n",
        value
    );
}


static ssize_t invert_store(
    struct device *dev,
    struct device_attribute *attr,
    const char *buf,
    size_t count)
{
    bool value;
    int ret;

    ret = kstrtobool(
        buf,
        &value
    );

    if (ret)
        return ret;


    mutex_lock(&state->lock);
    state->invert = value;
    mutex_unlock(&state->lock);


    pr_info(
        DRIVER_NAME
        ": control: invert=%d ctx=%s[%d]\n",
        value,
        current->comm,
        current->pid
    );


    /*
     * Synchronize immediately.
     */
    gpioctrl_sample_once(true);

    return count;
}

static DEVICE_ATTR_RW(invert);


/* poll_ms */

static ssize_t poll_ms_show(
    struct device *dev,
    struct device_attribute *attr,
    char *buf)
{
    unsigned int value;

    mutex_lock(&state->lock);
    value = state->poll_ms;
    mutex_unlock(&state->lock);

    return sysfs_emit(
        buf,
        "%u\n",
        value
    );
}


static ssize_t poll_ms_store(
    struct device *dev,
    struct device_attribute *attr,
    const char *buf,
    size_t count)
{
    unsigned int value;
    int ret;

    ret = kstrtouint(
        buf,
        0,
        &value
    );

    if (ret)
        return ret;


    if (
        value < MIN_POLL_MS ||
        value > MAX_POLL_MS
    )
        return -ERANGE;


    mutex_lock(&state->lock);
    state->poll_ms = value;
    mutex_unlock(&state->lock);


    pr_info(
        DRIVER_NAME
        ": control: poll_ms=%u ctx=%s[%d]\n",
        value,
        current->comm,
        current->pid
    );


    /*
     * Reset the next deadline so the new interval
     * becomes visible quickly.
     */
    mod_delayed_work(
        system_wq,
        &gpio_work,
        msecs_to_jiffies(value)
    );


    return count;
}

static DEVICE_ATTR_RW(poll_ms);


/*
 * sysfs attribute group.
 */

static struct attribute *gpioctrl_attrs[] = {
    &dev_attr_button.attr,
    &dev_attr_led.attr,

    &dev_attr_samples.attr,
    &dev_attr_changes.attr,
    &dev_attr_output_updates.attr,
    &dev_attr_uptime_ms.attr,

    &dev_attr_invert.attr,
    &dev_attr_poll_ms.attr,

    NULL
};


static const struct attribute_group gpioctrl_attr_group = {
    .attrs = gpioctrl_attrs,
};


/*
 * --------------------------------------------------------------------------
 * CHARACTER DEVICE
 * --------------------------------------------------------------------------
 *
 * misc_register() gives us:
 *
 *     /dev/gpioctrl
 *
 * Reading:
 *
 *     cat /dev/gpioctrl
 *
 * Writing:
 *
 *     echo "invert=1" > /dev/gpioctrl
 *     echo "poll_ms=100" > /dev/gpioctrl
 *     echo "sync" > /dev/gpioctrl
 *     echo "reset_stats" > /dev/gpioctrl
 */


static int gpioctrl_open(
    struct inode *inode,
    struct file *file)
{
    pr_info(
        DRIVER_NAME
        ": ctl: open ctx=%s[%d]\n",
        current->comm,
        current->pid
    );

    return 0;
}


static int gpioctrl_release(
    struct inode *inode,
    struct file *file)
{
    pr_info(
        DRIVER_NAME
        ": ctl: close ctx=%s[%d]\n",
        current->comm,
        current->pid
    );

    return 0;
}


static ssize_t gpioctrl_read(
    struct file *file,
    char __user *buf,
    size_t count,
    loff_t *ppos)
{
    char kbuf[512];

    int len;

    int button_value;
    int led_value;

    bool invert;
    unsigned int poll_ms;

    u64 samples;
    u64 changes;
    u64 updates;
    u64 loaded_at;
    u64 uptime;

    pid_t init_pid;
    pid_t last_pid;

    char init_comm[TASK_COMM_LEN];
    char last_comm[TASK_COMM_LEN];


    mutex_lock(&state->lock);

    button_value = state->button;
    led_value = state->led;

    invert = state->invert;
    poll_ms = state->poll_ms;

    samples = state->samples;
    changes = state->changes;
    updates = state->output_updates;

    loaded_at = state->loaded_at_ns;

    init_pid = state->init_pid;

    strscpy(
        init_comm,
        state->init_comm,
        TASK_COMM_LEN
    );

    last_pid = state->last_sample_pid;

    strscpy(
        last_comm,
        state->last_sample_comm,
        TASK_COMM_LEN
    );

    mutex_unlock(&state->lock);


    uptime =
        ktime_get_ns() -
        loaded_at;


    len = scnprintf(
        kbuf,
        sizeof(kbuf),

        "driver=%s\n"
        "gpiochip=%s\n"
        "button_offset=%d\n"
        "led_offset=%d\n"
        "button=%d\n"
        "led=%d\n"
        "invert=%d\n"
        "poll_ms=%u\n"
        "samples=%llu\n"
        "changes=%llu\n"
        "output_updates=%llu\n"
        "uptime_ms=%llu\n"
        "init_ctx=%s[%d]\n"
        "last_sample_ctx=%s[%d]\n",

        DRIVER_NAME,
        gpio_label,

        button_offset,
        led_offset,

        button_value,
        led_value,

        invert,
        poll_ms,

        (unsigned long long)samples,
        (unsigned long long)changes,
        (unsigned long long)updates,

        (unsigned long long)(
            uptime / 1000000ULL
        ),

        init_comm,
        init_pid,

        last_comm,
        last_pid
    );


    return simple_read_from_buffer(
        buf,
        count,
        ppos,
        kbuf,
        len
    );
}


static ssize_t gpioctrl_write(
    struct file *file,
    const char __user *buf,
    size_t count,
    loff_t *ppos)
{
    char kbuf[64];
    char *cmd;

    unsigned int poll_value;
    bool bool_value;

    int ret;


    if (
        count == 0 ||
        count >= sizeof(kbuf)
    )
        return -E2BIG;


    if (
        copy_from_user(
            kbuf,
            buf,
            count
        )
    )
        return -EFAULT;


    kbuf[count] = '\0';

    cmd = strim(kbuf);


    /*
     * sync
     */
    if (sysfs_streq(cmd, "sync")) {

        ret = gpioctrl_sample_once(true);

        if (ret < 0)
            return ret;

        return count;
    }


    /*
     * reset_stats
     */
    if (
        sysfs_streq(
            cmd,
            "reset_stats"
        )
    ) {
        mutex_lock(&state->lock);

        state->samples = 0;
        state->changes = 0;
        state->output_updates = 0;

        mutex_unlock(&state->lock);


        pr_info(
            DRIVER_NAME
            ": control: statistics reset ctx=%s[%d]\n",
            current->comm,
            current->pid
        );

        return count;
    }


    /*
     * invert=0
     * invert=1
     */
    if (
        !strncmp(
            cmd,
            "invert=",
            7
        )
    ) {
        ret = kstrtobool(
            cmd + 7,
            &bool_value
        );

        if (ret)
            return ret;


        mutex_lock(&state->lock);
        state->invert = bool_value;
        mutex_unlock(&state->lock);


        pr_info(
            DRIVER_NAME
            ": control: invert=%d ctx=%s[%d]\n",
            bool_value,
            current->comm,
            current->pid
        );


        gpioctrl_sample_once(true);

        return count;
    }


    /*
     * poll_ms=100
     */
    if (
        !strncmp(
            cmd,
            "poll_ms=",
            8
        )
    ) {
        ret = kstrtouint(
            cmd + 8,
            0,
            &poll_value
        );

        if (ret)
            return ret;


        if (
            poll_value < MIN_POLL_MS ||
            poll_value > MAX_POLL_MS
        )
            return -ERANGE;


        mutex_lock(&state->lock);
        state->poll_ms = poll_value;
        mutex_unlock(&state->lock);


        mod_delayed_work(
            system_wq,
            &gpio_work,
            msecs_to_jiffies(
                poll_value
            )
        );


        pr_info(
            DRIVER_NAME
            ": control: poll_ms=%u ctx=%s[%d]\n",
            poll_value,
            current->comm,
            current->pid
        );


        return count;
    }


    return -EINVAL;
}


static const struct file_operations gpioctrl_fops = {
    .owner = THIS_MODULE,

    .open = gpioctrl_open,
    .release = gpioctrl_release,

    .read = gpioctrl_read,
    .write = gpioctrl_write,

};


static struct miscdevice gpioctrl_miscdev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = DRIVER_NAME,
    .fops = &gpioctrl_fops,
    .mode = 0660,
};


/*
 * --------------------------------------------------------------------------
 * MODULE INITIALIZATION
 * --------------------------------------------------------------------------
 */

static int __init gpioctrl_init(void)
{
    u64 start_ns;
    u64 duration_ns;

    int button_value;
    int initial_led;

    int ret;


    start_ns = ktime_get_ns();


    /*
     * Dynamic kernel allocation.
     *
     * kzalloc:
     *
     *   - allocates kernel memory
     *   - clears the allocation to zero
     */
    state = kzalloc(
        sizeof(*state),
        GFP_KERNEL
    );

    if (!state)
        return -ENOMEM;


    mutex_init(
        &state->lock
    );


    state->button = -1;
    state->led = -1;

    state->poll_ms =
        initial_poll_ms;

    state->invert =
        initial_invert;

    state->loaded_at_ns =
        start_ns;


    state->init_pid =
        current->pid;

    strscpy(
        state->init_comm,
        current->comm,
        TASK_COMM_LEN
    );


    pr_info(
        DRIVER_NAME
        ": init: ctx=%s[%d] state=%p size=%zu "
        "gpiochip=%s button=%d led=%d poll=%ums invert=%d\n",

        current->comm,
        current->pid,

        state,
        sizeof(*state),

        gpio_label,
        button_offset,
        led_offset,

        state->poll_ms,
        state->invert
    );


    /*
     * Find gpio-sim.
     */
    gdev =
        gpio_device_find_by_label(
            gpio_label
        );

    if (!gdev) {
        ret = -ENODEV;
        goto err_free_state;
    }


    /*
     * Get descriptors.
     */
    button =
        gpio_device_get_desc(
            gdev,
            button_offset
        );

    if (IS_ERR(button)) {
        ret = PTR_ERR(button);
        button = NULL;

        goto err_put_gpio;
    }


    led =
        gpio_device_get_desc(
            gdev,
            led_offset
        );

    if (IS_ERR(led)) {
        ret = PTR_ERR(led);
        led = NULL;

        goto err_put_gpio;
    }


    /*
     * Input direction.
     */
    ret =
        gpiod_direction_input(
            button
        );

    if (ret)
        goto err_put_gpio;


    /*
     * Read initial button.
     */
    button_value =
        gpiod_get_value_cansleep(
            button
        );

    if (button_value < 0) {
        ret = button_value;
        goto err_put_gpio;
    }


    initial_led =
        state->invert ?
        !button_value :
        button_value;


    /*
     * Output direction + initial LED state.
     */
    ret =
        gpiod_direction_output(
            led,
            initial_led
        );

    if (ret)
        goto err_put_gpio;


    mutex_lock(&state->lock);

    state->button =
        button_value;

    state->led =
        initial_led;

    state->samples = 1;

    state->last_sample_pid =
        current->pid;

    strscpy(
        state->last_sample_comm,
        current->comm,
        TASK_COMM_LEN
    );

    mutex_unlock(&state->lock);


    /*
     * Create work item.
     */
    INIT_DELAYED_WORK(
        &gpio_work,
        gpioctrl_work_fn
    );


    /*
     * Register /dev/gpioctrl.
     */
    ret =
        misc_register(
            &gpioctrl_miscdev
        );

    if (ret)
        goto err_led_off;


    /*
     * Add sysfs files.
     */
    ret =
        sysfs_create_group(
            &gpioctrl_miscdev.this_device->kobj,
            &gpioctrl_attr_group
        );

    if (ret)
        goto err_misc;


    /*
     * Start polling.
     */
    schedule_delayed_work(
        &gpio_work,
        msecs_to_jiffies(
            state->poll_ms
        )
    );


    duration_ns =
        ktime_get_ns() -
        start_ns;


    pr_info(
        DRIVER_NAME
        ": state: button=%d led=%d\n",
        button_value,
        initial_led
    );


    pr_info(
        DRIVER_NAME
        ": init: completed in %llu us\n",
        (unsigned long long)(
            duration_ns / 1000ULL
        )
    );


    return 0;


err_misc:

    misc_deregister(
        &gpioctrl_miscdev
    );


err_led_off:

    gpiod_set_value_cansleep(
        led,
        0
    );


err_put_gpio:

    if (gdev)
        gpio_device_put(gdev);


err_free_state:

    pr_err(
        DRIVER_NAME
        ": init: failed err=%d\n",
        ret
    );

    kfree(state);

    state = NULL;
    gdev = NULL;
    button = NULL;
    led = NULL;

    return ret;
}


/*
 * --------------------------------------------------------------------------
 * MODULE UNLOAD
 * --------------------------------------------------------------------------
 */

static void __exit gpioctrl_exit(void)
{
    u64 start_ns;
    u64 duration_ns;
    u64 uptime_ns;

    u64 samples;
    u64 changes;
    u64 updates;

    int button_value;
    int led_value;


    start_ns =
        ktime_get_ns();


    /*
     * VERY IMPORTANT:
     *
     * Ensure no worker can execute module code after we
     * begin freeing resources.
     */
    cancel_delayed_work_sync(
        &gpio_work
    );


    /*
     * Remove userspace-facing sysfs controls before
     * releasing state.
     */
    sysfs_remove_group(
        &gpioctrl_miscdev.this_device->kobj,
        &gpioctrl_attr_group
    );


    /*
     * Remove /dev/gpioctrl.
     */
    misc_deregister(
        &gpioctrl_miscdev
    );


    button_value =
        gpiod_get_value_cansleep(
            button
        );

    led_value =
        gpiod_get_value_cansleep(
            led
        );


    /*
     * Leave output in a known state.
     */
    gpiod_set_value_cansleep(
        led,
        0
    );


    mutex_lock(&state->lock);

    state->led = 0;

    samples =
        state->samples;

    changes =
        state->changes;

    updates =
        state->output_updates;

    uptime_ns =
        start_ns -
        state->loaded_at_ns;

    mutex_unlock(&state->lock);


    pr_info(
        DRIVER_NAME
        ": exit: ctx=%s[%d] button=%d led=%d "
        "samples=%llu changes=%llu updates=%llu "
        "uptime=%llu ms\n",

        current->comm,
        current->pid,

        button_value,
        led_value,

        (unsigned long long)samples,
        (unsigned long long)changes,
        (unsigned long long)updates,

        (unsigned long long)(
            uptime_ns / 1000000ULL
        )
    );


    /*
     * Release GPIO subsystem reference.
     */
    gpio_device_put(
        gdev
    );


    pr_info(
        DRIVER_NAME
        ": memory: freeing state=%p size=%zu\n",
        state,
        sizeof(*state)
    );


    /*
     * Release dynamically allocated state.
     */
    kfree(state);


    state = NULL;
    gdev = NULL;
    button = NULL;
    led = NULL;


    duration_ns =
        ktime_get_ns() -
        start_ns;


    pr_info(
        DRIVER_NAME
        ": exit: completed in %llu us\n",
        (unsigned long long)(
            duration_ns / 1000ULL
        )
    );
}


module_init(gpioctrl_init);
module_exit(gpioctrl_exit);


MODULE_LICENSE("GPL");
MODULE_AUTHOR("rockun");
MODULE_DESCRIPTION(
    "GPIO simulator driver for kernel memory and lifecycle experiments"
);