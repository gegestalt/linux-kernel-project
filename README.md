# linux-kernel-project

A personal, build-up-in-order lab for learning Linux kernel module development.
Every numbered directory is a self-contained out-of-tree kernel module (or a
small family of them) that isolates one or two new concepts on top of
everything that came before it. Read them in order the first time through;
after that, treat the directory list as a reference you can jump back into.

There is no "final" module you're building toward. The point of the repo is
the sequence itself — each lab is a rung, not a step toward a specific ladder
top. New rungs get added as the concepts worth isolating keep surfacing.

## How the labs are organized

Each lab directory is independent: its own `Makefile`, its own `.c` file(s),
its own `README.md`. `make` in any lab directory builds only that lab. Nothing
is shared between labs except the pattern you carry forward from one to the
next, and (from lab 03 onward) the vendored kernel source in
[`../linux_mainline`](../linux_mainline), used for `checkpatch.pl` and for
cross-referencing subsystem source when a lab's README points at it.

```
NN_name/
├── README.md      # what this lab demonstrates, and how to run it
├── Makefile        # out-of-tree Kbuild wrapper (make / make clean)
├── *.c             # the module (and, in some labs, a userspace test helper)
└── (build output)   # *.ko, *.o, etc. — gitignored, produced by `make`
```

## The path

| # | Lab | What it isolates |
|---|-----|-------------------|
| [01](01_hello_init/) | `hello_init` | The absolute minimum module, using the legacy `init_module()`/`cleanup_module()` entry points. |
| [02](02_better_hello/) | `better_hello` | The modern `module_init()`/`module_exit()` macros, `__init`/`__exit` section attributes, and module metadata macros. |
| [03](03_gpio_sim/) | `gpio_sim` | A full-sized driver over Linux's `gpio-sim` virtual hardware: GPIO descriptors, a misc character device, sysfs attributes, module parameters, workqueues, mutex-protected state, and kernel memory allocation, all working together. Treat it as a preview of where the rest of the labs are individually headed. |
| [04](04_module_params/) | `module_params` | `module_param()` in isolation: types, permission bits, `/sys/module/*/parameters/`, and why parameters are read-only-by-default at runtime. |
| [05](05_register_cdev/) | `register_cdev` | Registering a character device with a dynamic major number and implementing `read()`, with `checkpatch.pl` wired into the Makefile. |
| [06](06_procfs_seqfile/) | `procfs_seqfile` | The `/proc` filesystem: a single-value proc file, then a multi-record listing built with the `seq_file` iterator API. |
| [07](07_log_level/) | `log_level` | `printk()`/`pr_*()` priority levels and how `dmesg`/`journalctl` filtering and the console loglevel interact with them. |
| [08](08_open_release_cdev/) | `open_release_cdev` | The `struct file` lifecycle: `open()`/`release()`, per-open state in `private_data`, and how `dup()` affects reference counting. |
| [09](09_read_write_cdev/) | `read_write_cdev` | A real read/write character device: a kernel-side buffer, `copy_to_user()`/`copy_from_user()`, `*offset` bookkeeping, and short reads/writes. |
| [10](10_ioctl_basics/) | `ioctl_basics` | Custom control operations with `unlocked_ioctl()`, the `_IO`/`_IOR`/`_IOW`/`_IOWR` macro family, and a userspace helper that drives them. |
| [11](11_concurrency_locking/) | `concurrency_locking` | A deliberately racy shared counter, then the same counter fixed three ways: `spinlock_t`, `struct mutex`, and `atomic_t` — with a userspace stress test that makes the race visible. |
| [12](12_wait_queues_blocking/) | `wait_queues_blocking` | Blocking reads with `wait_event_interruptible()`, waking waiters from a timer, and `poll()`/`select()` support via `poll_wait()`. |
| [13](13_kernel_memory/) | `kernel_memory` | `kmalloc()`/`kzalloc()` vs `vmalloc()` vs a dedicated `kmem_cache`, exposed through sysfs so you can compare behavior and failure modes directly. |
| [14](14_timers_workqueues/) | `timers_workqueues` | `struct timer_list` (atomic/softirq context) side by side with `struct work_struct` (process context), and why the distinction matters for what each callback is allowed to do. |
| [15](15_kthreads/) | `kthreads` | A dedicated kernel thread (`kthread_run()`) producing data that a reader consumes through the character device, plus clean shutdown with `kthread_stop()`. |
| [16](16_debugfs_sysfs/) | `debugfs_sysfs` | `debugfs_create_*()` next to the sysfs attributes from lab 03/09, contrasting the two: stability guarantees, discoverability, and when to use which. |
| [17](17_next_steps/) | `next_steps` | No new module — a map of where to go once these labs stop being new: interrupts, platform drivers & devicetree overlays, DMA, block drivers, tracing, and how to move this whole workflow into a real or QEMU-emulated board instead of `gpio-sim`. |

## Environment

All labs were built and tested against the running kernel's own headers:

```bash
uname -r
ls /lib/modules/$(uname -r)/build   # must exist — install linux-headers-$(uname -r) if not
```

`checkpatch.pl`-backed `make check` targets (labs 05, 07, 08, and onward) use
the vendored source tree at `../linux_mainline` for the script itself and for
its `--root` (spelling dictionary, MAINTAINERS, etc). That tree is *not*
built — it's only there as a source and lint reference.

### A word on safety

These are loadable kernel modules: a bug in any of them can panic or wedge
the machine, not just crash a process. That's an acceptable, expected risk
for `01`–`08`, which touch nothing but their own state. From **lab 11
onward** (locking, blocking I/O, kernel threads) bugs are much easier to turn
into a hang or a genuine race, and lab 13 deliberately explores allocator
edge cases. **Run this repository inside a disposable VM**, not on bare
metal you care about. A quick option:

```bash
# from the host, once:
sudo apt install qemu-system-x86 debootstrap   # or use an existing cloud image
# then build/copy this repo into the guest and do everything inside it
```

If you're on a machine where `uname -r` already reports a VM kernel (as this
one does), you're already set up correctly — just keep it that way.

## Everyday workflow

```bash
cd NN_lab_name
make                                   # build the .ko (and any test helper)
sudo insmod ./module_name.ko           # load
dmesg -w                                # watch kernel log output, in another shell
# ... interact with the module: sysfs, /dev, /proc, ioctl ...
sudo rmmod module_name                 # unload
make clean                             # remove build output
```

Each lab's own `README.md` has the exact commands for that lab — device
names, module parameters, sysfs paths, and what output to expect — so you
don't have to reconstruct them from the source every time.

## Contributing to your own repo

The git history here is meant to double as part of the learning record: one
focused branch and PR per lab or per fix, same as the existing history
(`feature/register-cdev-checkpatch`, `fix/issue-3-printk-checkpatch`, ...).
Keep doing that going forward — small, reviewable, one concept at a time.
