# GDB walkthrough — 03_gpio_sim

`gpioctrl.c` is the first module with real runtime complexity: it owns a
`kzalloc()`'d state struct guarded by a mutex, talks to the `gpio-sim`
subsystem through GPIO descriptors, and re-arms a `delayed_work` item
forever to poll a simulated button and drive a simulated LED. Unlike
modules 01/02, most of this driver's interesting code does **not** run
during `insmod` at all — `gpioctrl_work_fn()` runs later, from a
`kworker` thread, asynchronously, driven by the workqueue rather than
by anything you typed. That makes this the right module to learn a
technique the earlier ones didn't need: catching a breakpoint that
fires on its own, on a schedule, from a thread you didn't start.

## Prerequisite: gpio-sim must exist before this module can init

`gpioctrl_init()` calls `gpio_device_find_by_label(gpio_label)` (default
`"gpio-sim.0:node0"`) and fails with `-ENODEV` if nothing matches — this
was confirmed live the hard way: `insmod`ing `gpioctrl.ko` with nothing
set up first fails immediately, twice (the driver retries once), with
exactly that `-ENODEV`. `CONFIG_GPIO_SIM=m` — a *module*, not built into
the kernel image — so `gpio-sim.ko` has to actually be loaded before
`gpioctrl.ko` can find it.

**`modprobe gpio-sim` does not work in this repo's QEMU/busybox
environment** — confirmed live: `modprobe: can't change directory to
'/lib/modules': No such file or directory`. The minimal busybox
initramfs this repo's [`../gdb_debugging.md`](../gdb_debugging.md)
builds has no `/lib/modules` tree for `modprobe` to search (that's a
real difference from a normal Linux host, where [`readme.md`](readme.md)'s
own "Set up the simulated hardware first" section — written for running
this lab directly on your own machine, not inside this debug VM — using
`modprobe` is correct). Inside the QEMU guest, `insmod` the built
`gpio-sim.ko` directly instead, from the same disk image `gpioctrl.ko`
itself is copied onto (see Environment below):

```bash
# vmb, before insmod gpioctrl.ko:
insmod /mnt/labs/03_gpio_sim/gpio-sim.ko
mount -t configfs configfs /sys/kernel/config
mkdir -p /sys/kernel/config/gpio-sim/gpio-device/node0
echo 22 > /sys/kernel/config/gpio-sim/gpio-device/node0/num_lines
echo 1 > /sys/kernel/config/gpio-sim/gpio-device/live
cat /sys/kernel/config/gpio-sim/gpio-device/dev_name   # confirm it's "gpio-sim.0"
```

(`gpiodetect` isn't in this repo's minimal busybox build — skip it, it's
a convenience check on a real host, not required for the module to
load.) If `dev_name` doesn't read exactly `gpio-sim.0` (possible if
another gpio-sim device already exists this boot), pass the real label
at load time: `insmod gpioctrl.ko gpio_label="<real-name>:node0"
button_offset=20 led_offset=21`.

## Environment

```bash
cd 03_gpio_sim
make -C /home/adiopocere/Desktop/codes/linux_mainline M=$(pwd) modules
modinfo gpioctrl.ko | grep vermagic
sudo mount -o loop /home/adiopocere/Desktop/codes/qemu-vmb/labs-disk.img /tmp/vmb-mnt
sudo mkdir -p /tmp/vmb-mnt/03_gpio_sim
sudo cp gpioctrl.ko /tmp/vmb-mnt/03_gpio_sim/
sudo cp /home/adiopocere/Desktop/codes/linux_mainline/drivers/gpio/gpio-sim.ko /tmp/vmb-mnt/03_gpio_sim/
sudo umount /tmp/vmb-mnt
```

`gpio-sim.ko` is built as part of the debug kernel tree itself (it's a
real upstream driver, not something this repo owns) — it isn't produced
by this lab's own `make`, so it has to be copied from
`linux_mainline/drivers/gpio/gpio-sim.ko` explicitly, alongside
`gpioctrl.ko`, rather than built here.

## tmux layout

Standard `vmb` + `gdbsess` pair, see
[`../gdb_debugging.md`](../gdb_debugging.md). `target remote :1234`,
`lx-version`, then break `do_init_module` and `insmod` as in every
earlier module to get `lx-symbols` to see this one. This walkthrough
picks up from there.

## Real, verified breakpoint targets

Confirmed statically against the built module (your own build will
match these exactly — `gpioctrl.c` is unmodified across builds):

```
Line 1056: gpioctrl_init          (init/exit lifecycle)
Line 215:  gpioctrl_sample_once   (the shared read-modify-write core)
Line 856:  gpioctrl_write         (/dev/gpioctrl command parser)
Line 324:  gpioctrl_work_fn       (the recurring, self-scheduling poll)
Line 534:  invert_store           (sysfs RW attribute)
Line 598:  poll_ms_store          (sysfs RW attribute)
```

(`gpioctrl_exit`'s *static* `info line` — run against the standalone
`.ko` file, before any of this — resolves into
`include/linux/timekeeping.h` rather than `gpioctrl.c`, because its
very first executed line is the inlined `ktime_get_ns()` call that
opens the function. That's a fine, accurate offline fact about the
binary. **What it does not mean is that `break gpioctrl_exit` will
actually stop there once the module is loaded live** — see the Cleanup
section below for why, confirmed live, it doesn't.)

### Step 1 — init: watch state get built, then confirm the polling starts

```gdb
(gdb) break gpioctrl_init
(gdb) continue
```
```bash
# vmb, after gpio-sim setup above:
insmod /mnt/labs/03_gpio_sim/gpioctrl.ko
```
```gdb
Thread 2 hit Breakpoint N, gpioctrl_init () at gpioctrl.c:1056
(gdb) next        # repeat past the kzalloc() call
(gdb) print state
$1 = (struct gpioctrl_state *) 0x...
(gdb) print *state
$2 = {lock = {...}, button = 0, led = 0, invert = false, poll_ms = 0, ...}
```

This is memory the allocator just handed back — `kzalloc()` zeroed it,
so every field reads as 0/false/NULL right now, *before* any of the
init function's own assignments below have run. Step forward past
`mutex_init(&state->lock)` and the `state->button = -1;` /
`state->poll_ms = initial_poll_ms;` block, then re-`print *state` — the
values now match what actually got assigned, proving the zero-init
you saw a moment ago wasn't lucky, it was `kzalloc()`'s contract.

Step through to the GPIO descriptor lookups:

```gdb
(gdb) next    # through gpio_device_find_by_label
(gdb) print gdev
(gdb) next    # through gpio_device_get_desc(gdev, button_offset)
(gdb) print button
(gdb) next    # led descriptor
(gdb) print led
```

If any of these prints an `IS_ERR()`-shaped pointer (a small negative
value cast to a pointer, e.g. `0xfffffffffffffea`) instead of a real
address, that's exactly the `err_put_gpio`/`err_free_state` path about
to be taken — `next` a couple more times and watch `ret` get set from
`PTR_ERR()` before the function returns it. This is a fast, honest way
to see *why* an `insmod` failed without adding a single extra
`pr_err()` to the driver.

Continue to the end and confirm the delayed work got armed:

```gdb
(gdb) finish
(gdb) lx-dmesg
```

You should see this module's own `pr_info(DRIVER_NAME ": init: completed
in ... us\n")` line at the very end of the ring buffer.

### Step 2 — catch the workqueue callback firing on its own

This is the technique modules 01/02 never needed. `gpioctrl_work_fn()`
isn't called by anything you're about to type — it fires because
`schedule_delayed_work()` armed it during init, on a `poll_ms`
timer (default 500ms). Just set the breakpoint and `continue`; **do
nothing else in `vmb`** and it will still hit, on its own schedule:

```gdb
(gdb) break gpioctrl_work_fn
(gdb) continue
```

Within roughly half a second (or whatever `poll_ms` is currently set
to):

```
Thread 2 hit Breakpoint N, gpioctrl_work_fn (work=0x...) at gpioctrl.c:324
(gdb) bt
#0  gpioctrl_work_fn (work=0x...) at gpioctrl.c:324
#1  0x... in process_one_work (worker=0x..., work=0x...) at kernel/workqueue.c:...
#2  0x... in worker_thread (...) at kernel/workqueue.c:...
#3  0x... in kthread (...) at kernel/kthread.c:...
```

**Read this backtrace carefully — it's the whole point of this module.**
There is no `insmod`, `rmmod`, or any userspace syscall anywhere in
this stack. The bottom frames are generic kernel workqueue machinery
(`process_one_work`, `worker_thread`, `kthread`) — a dedicated kernel
thread whose entire job is pulling queued work items and running them.
Confirm which thread you're actually in:

```gdb
(gdb) lx-ps
```

Find the line whose pid matches `$lx_current()->pid` — its `comm`
field will be something like `kworker/0:2`, never `insmod` or a shell.

```gdb
(gdb) next     # through gpioctrl_sample_once(false)
(gdb) print ret
(gdb) next     # through gpioctrl_get_poll_ms()
(gdb) print delay
(gdb) finish   # back into process_one_work
```

Every time you `continue` again from here, you'll hit the same
breakpoint again on the next tick — this driver's poll loop, live,
one iteration per stop, for as long as you keep continuing.

### Step 3 — the shared core: `gpioctrl_sample_once`

Both the workqueue path (step 2) and a manual `/dev/gpioctrl` write
("`sync`" command, see below) funnel through this one function — it's
the natural place to inspect the mutex-protected read-modify-write at
the heart of this driver, the same pattern 11_concurrency_locking dedicates
a whole module to, seen here in a real driver rather than an artificial
stress test:

```gdb
(gdb) break gpioctrl_sample_once
(gdb) continue
```

Let it hit from the workqueue (no action needed), then step through:

```gdb
(gdb) next            # gpiod_get_value_cansleep(button)
(gdb) print button_value
(gdb) next             # mutex_lock(&state->lock) - now stepped over it
(gdb) print state->button      # the *previous* sample, still in state
(gdb) next               # desired_led computed
(gdb) print desired_led
(gdb) print changed        # true only if button_value != the previous state->button
```

Toggle the simulated button from `vmb` between two `continue`s to force
`changed` to actually be `true` on a later hit — this is what makes the
LED output line (`gpiod_set_value_cansleep`) actually execute instead
of being skipped, since the driver only touches the output "when
required" (see the comment in the source). `gpio-sim` exposes each
simulated line's own control file one level below the `/dev/gpiochipN`
node it created:

```bash
# vmb:
GPIOCHIP=$(gpiodetect | grep gpio-sim | awk '{print $1}')
echo pull-up | tee /sys/devices/platform/gpio-sim.0/$GPIOCHIP/sim_gpio20/pull
```

### Step 4 — `/dev/gpioctrl` write path and sysfs stores

```gdb
(gdb) break gpioctrl_write
(gdb) continue
```
```bash
# vmb:
echo "poll_ms=100" > /dev/gpioctrl
```
```gdb
Thread 2 hit Breakpoint N, gpioctrl_write (...) at gpioctrl.c:856
(gdb) next            # past copy_from_user
(gdb) print kbuf
(gdb) next             # past strim()
(gdb) print cmd
```

`cmd` now holds `"poll_ms=100"` as a real, null-terminated kernel
string you can `print` directly — copy_from_user() already moved it
across the user/kernel boundary by this point, which is exactly why
breaking any earlier than this line would show you garbage or
uninitialized stack instead.

`invert_store`/`poll_ms_store` (the sysfs equivalents, verified at
lines 534/598) follow the identical shape — `break invert_store`,
`echo 1 | sudo tee /sys/class/misc/gpioctrl/invert`, and note this path
calls `gpioctrl_sample_once(true)` at the end (forcing an immediate
resync) — step into it with `step` instead of `next` at that line to
follow the call rather than skip over it.

## Cleanup

**`break gpioctrl_exit` accepts with no error but never fires** —
confirmed live, the same underlying cause called out above:
`gpioctrl_exit` is `__exit`, placed in the `.exit.text` ELF section,
which `lx-symbols` never relocates (full diagnosis, straight from this
kernel's `scripts/gdb/linux/symbols.py`, in module 02's and 12's
walkthroughs). The breakpoint resolves to a raw file offset instead of
a real kernel address; `rmmod` completes underneath it while GDB sits
at `Continuing.` forever.

**Reaching `cleanup_module` itself, the working fix**:

```gdb
(gdb) delete
(gdb) break __do_sys_delete_module
(gdb) continue
```
```bash
# vmb:
rmmod gpioctrl
```
```gdb
Thread N hit Breakpoint N, __do_sys_delete_module (...) at kernel/module/main.c:808
(gdb) advance kernel/module/main.c:863
863         mod->exit();
(gdb) print mod->exit
$1 = (void (*)(void)) 0xffff80007c32c2e0
(gdb) add-symbol-file /home/adiopocere/Desktop/codes/linux-kernel-project/03_gpio_sim/gpioctrl.ko -s .exit.text 0xffff80007c32c2e0
(y or n) y
(gdb) break gpioctrl_exit
Breakpoint N at 0x100: gpioctrl_exit. (2 locations)
```

(The address is from one real run — always use whatever `print
mod->exit` gives you; module memory placement is random per boot even
with `nokaslr`.) Disable the broken location, keep the relocated one:

```gdb
(gdb) disable N.1
(gdb) continue
```
```bash
# vmb:
rmmod gpioctrl
```
```gdb
Thread N hit Breakpoint N.2, 0xffff80007c32c2e8 in cleanup_module ()
```

**Reaching `cancel_delayed_work_sync()` specifically — a simpler,
independent technique.** `cleanup_module`'s own body has no line-by-line
resolution (same as everywhere else in this repo), and live-tested,
`next` from its entry ran straight through the entire function in one
step rather than landing inside a call the way it does for some other
modules — which call a `next` lands you in depends on the exact
compiled instruction layout, not something worth relying on. Since
`cancel_delayed_work_sync()` is an ordinary, fully-resolved vmlinux
function (no relocation issue — only `__exit`-marked *module* code has
this problem), break on it directly instead, before continuing past the
`__do_sys_delete_module` entry hit — no `add-symbol-file` needed for
this part at all:

```gdb
(gdb) break __do_sys_delete_module
(gdb) break cancel_delayed_work_sync
(gdb) continue
```
```bash
# vmb:
rmmod gpioctrl
```
```gdb
Thread N hit Breakpoint N, __do_sys_delete_module (...) at kernel/module/main.c:808
(gdb) continue
Thread N hit Breakpoint N, cancel_delayed_work_sync (dwork=0x...) at kernel/workqueue.c:4632
```

Confirmed live: continuing straight from the syscall entry lands
exactly here, with a real name, real line, and full `bt`/`next`/
`finish` support — no section-relocation workaround required, because
this code lives in `vmlinux` itself, not in the module's own
`.exit.text`.

**Why this line matters.** `cancel_delayed_work_sync()` blocks until
any *currently running* `gpioctrl_work_fn()` has finished — if `rmmod`
proceeded to `kfree(state)` while a workqueue callback was mid-flight,
that callback would dereference already-freed memory the instant it
resumed. To see this guarantee in action rather than just read the
source comment claiming it: set a separate breakpoint on
`gpioctrl_work_fn`, get it to hit, and *while still stopped there*, run
`rmmod gpioctrl` from `vmb` — it will hang (not crash, not fail —
genuinely block) until you `continue` past the work function, because
`cancel_delayed_work_sync()` is waiting on exactly that. `poweroff -f`
the guest afterward regardless of which path you took; a hung `rmmod`
during a debugging session isn't worth reasoning about further.

```bash
# vmb:
poweroff -f
```

## What this proves

Most of a real driver's interesting behavior does not run synchronously
underneath `insmod`/`rmmod`/a syscall you typed — it runs later, on
someone else's schedule (a workqueue, in this module; a timer in 14, a
dedicated kthread in 15), and a breakpoint doesn't care which: set it,
`continue`, and it fires whenever that code path actually runs,
identified afterward by backtrace and `lx-ps`, not by anything you did
to trigger it. The `cancel_delayed_work_sync()` exercise in cleanup is
the same lesson from the other direction — correct module unload has
to actively wait out exactly this kind of asynchronous execution before
freeing anything it might still touch.
