# linux-kernel-project

Hands-on Linux kernel module development, labs 01 through 16, each one
isolating a new concept on top of the ones before it. Go through them in
order the first time; after that it's a reference you jump back into.

## Where to find things

| Looking for... | Go to |
|---|---|
| What a specific lab does, how to build/run it | `NN_lab_name/README.md` |
| A live GDB session walking through that lab's code | `NN_lab_name/GDB_WALKTHROUGH.md` |
| Every lab's run commands in one place, no explanations | [`RUNBOOK.md`](RUNBOOK.md) |
| One-time setup for the GDB/QEMU debugging environment | [`GDB_DEBUGGING.md`](GDB_DEBUGGING.md) |
| Getting a module onto a real machine — DKMS, signing, upstreaming | [`DEPLOYMENT.md`](DEPLOYMENT.md) |

Each lab directory is self-contained: its own `Makefile` and `.c` file(s),
built independently with `make`. Nothing is shared between labs except the
vendored kernel source at [`../linux_mainline`](../linux_mainline), used for
`checkpatch.pl` linting and as a source reference.

```
NN_name/
├── README.md            # what this lab does, how to run it
├── GDB_WALKTHROUGH.md    # step-by-step GDB session for this lab
├── Makefile              # make / make clean / make check
├── *.c                   # the module (and, in some labs, a userspace test helper)
└── (build output)        # *.ko, *.o, etc. — gitignored
```

## The labs

| # | Lab | Concept |
|---|-----|---------|
| [01](01_hello_init/) | `hello_init` | Minimum module, legacy `init_module()`/`cleanup_module()`. |
| [02](02_better_hello/) | `better_hello` | `module_init()`/`module_exit()`, `__init`/`__exit`, module metadata. |
| [03](03_gpio_sim/) | `gpio_sim` | Full driver over `gpio-sim`: GPIO descriptors, misc device, sysfs, params, workqueues, locking, allocation — a preview of where the rest of the labs are headed. |
| [04](04_module_params/) | `module_params` | `module_param()`, permission bits, `/sys/module/*/parameters/`. |
| [05](05_register_cdev/) | `register_cdev` | Character device with a dynamic major number, `read()`. |
| [06](06_procfs_seqfile/) | `procfs_seqfile` | `/proc` files, single-value and `seq_file`-based multi-record listing. |
| [07](07_log_level/) | `log_level` | `printk()`/`pr_*()` levels, `dmesg`/console loglevel filtering. |
| [08](08_open_release_cdev/) | `open_release_cdev` | `open()`/`release()`, `private_data`, `dup()` and refcounting. |
| [09](09_read_write_cdev/) | `read_write_cdev` | Kernel-side buffer, `copy_to_user()`/`copy_from_user()`, offsets, short reads/writes. |
| [10](10_ioctl_basics/) | `ioctl_basics` | `unlocked_ioctl()`, the `_IO`/`_IOR`/`_IOW`/`_IOWR` macros, a userspace driver. |
| [11](11_concurrency_locking/) | `concurrency_locking` | A racy shared counter, then fixed three ways: spinlock, mutex, atomic. |
| [12](12_wait_queues_blocking/) | `wait_queues_blocking` | Blocking reads, `wait_event_interruptible()`, `poll()`/`select()`. |
| [13](13_kernel_memory/) | `kernel_memory` | `kmalloc()`/`kzalloc()` vs `vmalloc()` vs `kmem_cache`. |
| [14](14_timers_workqueues/) | `timers_workqueues` | `timer_list` (softirq context) vs `work_struct` (process context). |
| [15](15_kthreads/) | `kthreads` | A kernel thread producing data, `kthread_run()`/`kthread_stop()`. |
| [16](16_debugfs_sysfs/) | `debugfs_sysfs` | `debugfs_create_*()` next to sysfs from labs 03/09 — when to use which. |

## Build and run

```bash
uname -r
ls /lib/modules/$(uname -r)/build   # must exist — install linux-headers-$(uname -r) if not
```

```bash
cd NN_lab_name
make                        # build the .ko
sudo insmod ./module.ko     # load
dmesg -w                    # watch kernel log, in another shell
# interact with the module: sysfs, /dev, /proc, ioctl...
sudo rmmod module_name      # unload
make clean
```

Each lab's own `README.md` has the exact device names, params, and sysfs
paths for that lab.

## Run this in a VM

These are loadable kernel modules — a bug can panic or wedge the whole
machine, not just crash a process. Labs 01–08 only touch their own state, so
the risk is low. From **lab 11 onward** (locking, blocking I/O, kernel
threads) it's much easier to hit a real hang or race, and lab 13
deliberately explores allocator edge cases.

**Don't run this on bare metal you care about.** If `uname -r` doesn't
already report a VM kernel, set one up first — a disposable QEMU or VirtualBox
guest is enough.

## Contributing

One branch and PR per lab or per fix — small, reviewable, one concept at a
time.
