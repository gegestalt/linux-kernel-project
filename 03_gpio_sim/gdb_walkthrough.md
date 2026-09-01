# GDB walkthrough — 03_gpio_sim, hands-on, start to finish

`gpioctrl.c` is the first module with real runtime complexity: a
`kzalloc()`'d state struct guarded by a mutex, GPIO descriptors from the
`gpio-sim` subsystem, and a `delayed_work` item that re-arms itself
forever to poll a simulated button and drive a simulated LED. Unlike
modules 01/02, most of this driver's interesting code does **not** run
during `insmod` — `gpioctrl_work_fn()` runs later, from a `kworker`
thread, driven by the workqueue, not by anything you type. This
walkthrough's whole point is catching a breakpoint that fires on its
own, on a schedule, from a thread you never started.

Every command below says exactly which pane. One command per step,
always — paste it, wait for the prompt to come back, then the next one.

---

## Step 0 — start the tmux session

*Regular terminal, not tmux yet.*

```bash
tmux kill-session -t kgdb 2>/dev/null
tmux new-session -d -s kgdb -x 220 -y 50
tmux split-window -h -t kgdb
tmux set -g mouse on
tmux select-pane -t kgdb:0.0 -T vmb
tmux select-pane -t kgdb:0.1 -T gdb
tmux set -t kgdb pane-border-status top
tmux attach -t kgdb
```

Two panes now: **vmb** (left) and **gdb** (right).

## Step 1 — build it

*Regular terminal.*

```bash
cd /home/adiopocere/Desktop/codes/linux-kernel-project/03_gpio_sim
make -C /home/adiopocere/Desktop/codes/linux_mainline M=$(pwd) modules
```

`gpio-sim.ko` itself — the simulated GPIO backend this driver binds to —
isn't built by this step. It's a real upstream driver that's already
part of the debug kernel tree:

```bash
ls /home/adiopocere/Desktop/codes/linux_mainline/drivers/gpio/gpio-sim.ko
```

## Step 2 — verify the breakpoint targets, statically

```bash
gdb -q -batch -nx -ex "file gpioctrl.ko" \
    -ex "info line gpioctrl_init" -ex "info line gpioctrl_exit" \
    -ex "info line gpioctrl_write" -ex "info line gpioctrl_work_fn" \
    -ex "ptype struct gpioctrl_state" gpioctrl.ko
```
```
Line 1056 of "gpioctrl.c" starts at address 0x1dd0 <gpioctrl_init> ...
Line 175 of ".../timekeeping.h" starts at address 0x1c58 <gpioctrl_exit> ...
Line 856 of "gpioctrl.c" starts at address 0xce8 <gpioctrl_write> ...
Line 324 of "gpioctrl.c" starts at address 0xc10 <gpioctrl_work_fn> ...
type = struct gpioctrl_state {
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
    char init_comm[16];
    pid_t last_sample_pid;
    char last_sample_comm[16];
}
```

`gpioctrl_exit` resolving into `timekeeping.h` rather than `gpioctrl.c`
is a real, worth-knowing DWARF quirk, not a mistake: the function's
*very first* statement is `start_ns = ktime_get_ns();`, and
`ktime_get_ns()` is a `static inline` defined in the kernel's own
`timekeeping.h` — so the function's entry address maps to that inlined
callee's source line, not `gpioctrl_exit`'s own opening brace. `break
gpioctrl_exit` still sets a perfectly good breakpoint there; GDB is just
being literal about which source line owns the first machine
instruction.

## Step 3 — check vermagic, copy both `.ko` files onto the scratch disk

```bash
modinfo gpioctrl.ko | grep vermagic
```
```
vermagic: 7.2.0-kgdb-debug+ SMP preempt mod_unload modversions aarch64
```
```bash
sudo mount -o loop /home/adiopocere/Desktop/codes/qemu-vmb/labs-disk.img /tmp/vmb-mnt
sudo mkdir -p /tmp/vmb-mnt/03_gpio_sim
sudo cp gpioctrl.ko /tmp/vmb-mnt/03_gpio_sim/
sudo cp /home/adiopocere/Desktop/codes/linux_mainline/drivers/gpio/gpio-sim.ko /tmp/vmb-mnt/03_gpio_sim/
sudo umount /tmp/vmb-mnt
```

`gpio-sim.ko` is copied alongside `gpioctrl.ko` rather than built here —
it's a real upstream driver from the debug tree, not something this repo
owns.

## Step 4 — boot the guest

**Pane: vmb**

```bash
qemu-system-aarch64 -M virt -cpu max -m 1024 -smp 2 \
  -kernel /home/adiopocere/Desktop/codes/linux_mainline/arch/arm64/boot/Image \
  -initrd /home/adiopocere/Desktop/codes/qemu-vmb/initramfs.cpio.gz \
  -drive file=/home/adiopocere/Desktop/codes/qemu-vmb/labs-disk.img,if=virtio,format=raw \
  -append "console=ttyAMA0 rdinit=/init nokaslr" -nographic -s
```

Wait for `=== VM B (QEMU) ready ===` and `~ #`.

## Step 5 — bring up gpio-sim in the guest, before touching gpioctrl at all

`gpioctrl_init()` calls `gpio_device_find_by_label(gpio_label)` (default
`"gpio-sim.0:node0"`) and fails with `-ENODEV` if nothing matches yet —
`gpio-sim` has to exist first. This minimal busybox guest has no
`/lib/modules` tree, so `modprobe` doesn't work here (`modprobe: can't
change directory to '/lib/modules': No such file or directory`,
confirmed live) — `insmod` the built `.ko` directly instead:

**Pane: vmb**

```bash
insmod /mnt/labs/03_gpio_sim/gpio-sim.ko
```
```bash
mount -t configfs configfs /sys/kernel/config
```
```bash
mkdir -p /sys/kernel/config/gpio-sim/gpio-device/node0
```
```bash
echo 22 > /sys/kernel/config/gpio-sim/gpio-device/node0/num_lines
```
```bash
echo 1 > /sys/kernel/config/gpio-sim/gpio-device/live
```
```bash
cat /sys/kernel/config/gpio-sim/gpio-device/dev_name
```

Expect `gpio-sim.0`. (`gpiodetect` isn't in this minimal busybox build —
skip it; it's a convenience check on a real host, not required here. If
`dev_name` reads something other than `gpio-sim.0`, another gpio-sim
device already exists this boot — pass the real name at load time in
step 9: `gpio_label="<real-name>:node0"`.)

## Step 6 — start gdb, connect

**Pane: gdb**

```bash
cd /home/adiopocere/Desktop/codes/linux_mainline && gdb -q -iex 'set auto-load safe-path /' vmlinux
```
```
target remote :1234
```
```
lx-version
```

## Step 7 — break on the load entry point

**Pane: gdb**

```
break do_init_module
```
```
continue
```

Switch panes.

## Step 8 — trigger the load

**Pane: vmb**

```bash
insmod /mnt/labs/03_gpio_sim/gpioctrl.ko
```

## Step 9 — load symbols, break inside init

**Pane: gdb**

```
lx-symbols /home/adiopocere/Desktop/codes/linux-kernel-project
```

**`break gpioctrl_init` does not work here — confirmed live (this exact
mechanism, on two other modules), don't trust it.** `gpioctrl_init` is
`__init`, placed in `.init.text`. `lx-symbols` relocates only a fixed,
hardcoded list of sections (`scripts/gdb/linux/symbols.py`'s
`_section_arguments()`: `.data`, `.data..read_mostly`, `.rodata`,
`.bss`, `.text.hot`, `.text.unlikely`) — `.init.text` isn't one of them,
the same root cause as step 16's `.exit.text` problem below, just
hitting the *load* path instead. `break gpioctrl_init` right now would
be accepted with no error and silently resolve to a tiny, bogus,
unrelocated file offset — it would never actually fire; `insmod` would
run straight through to completion with nothing caught.

The fix mirrors step 16's exit-path fix exactly, using `mod->init`
instead of `mod->exit` — you're still stopped inside `do_init_module`
right now (step 7), before `do_one_initcall(mod->init)` has run, so
`mod->init` is already the module's real, live init-function address:

```
print mod->init
```
```
$1 = (int (*)(void)) 0x...
```

(That address is from one real run — module memory is placed fresh each
boot even with `nokaslr`. Use whatever `print mod->init` gives you
next.)

```
add-symbol-file /home/adiopocere/Desktop/codes/linux-kernel-project/03_gpio_sim/gpioctrl.ko -s .init.text 0x...
```
```
y
```
```
break gpioctrl_init
```
```
Breakpoint N at 0x...: gpioctrl_init. (2 locations)
```

Two locations — `N.1` is the old broken raw-offset one (matching what
plain `break gpioctrl_init` gave a moment ago), `N.2` is the
newly-relocated real one:

```
disable N.1
```
```
continue
```
```
Thread 2 hit Breakpoint N.2, 0x... in init_module ()
```

Reports as `init_module`, not `gpioctrl_init` — the same alias
mechanics documented in module 02's walkthrough (`module_init()`
generates a hard alias to the legacy name; both point at the identical
address, GDB just picked one label for this particular PC). Also worth
knowing before the next step: because `add-symbol-file` here only
supplies a symbol address, not full compiler-generated debug info,
GDB has nothing to attach a source line to at this exact PC — don't be
alarmed if `list`/frame output here doesn't show `gpioctrl.c:1056` the
way a normal breakpoint would. Stepping forward with `next` moves into
fully-resolved source immediately, so this only affects this one landing
point, not anything downstream in this walkthrough.

## Step 10 — watch `kzalloc()`'s zeroed memory get filled in

```
next
```
```
print state
```
```
$2 = (struct gpioctrl_state *) 0x...
```
```
print *state
```
```
$3 = {lock = {...}, button = 0, led = 0, invert = false, poll_ms = 0, ...}
```

**What this shows:** `kzalloc()` zeroed this memory — every field reads
0/false/NULL right now, *before* any of `gpioctrl_init`'s own assignments
below have run. Step forward past `mutex_init(&state->lock)` and the
`state->button = -1;` / `state->poll_ms = initial_poll_ms;` block:

```
next
```
```
next
```
```
print state->poll_ms
```
```
$4 = 500
```

Now matching `initial_poll_ms` (the module's parameter default) — proof
the zero-init a moment ago wasn't luck, it's `kzalloc()`'s actual
contract.

## Step 11 — step through the GPIO descriptor lookups

```
next
```
```
print gdev
```

Non-`NULL` if `gpio-sim`'s device was found by the exact label from step
5.

```
next
```
```
print button
```
```
next
```
```
print led
```

If either prints an `IS_ERR()`-shaped pointer (a small negative value
cast to a pointer, e.g. `0xfffffffffffffea`) instead of a real address,
that's the `err_put_gpio`/`err_free_state` path about to be taken —
`next` a couple more times and watch `ret` get set from `PTR_ERR()`
before the function returns it. A fast, honest way to see *why* an
`insmod` failed without adding a single extra `pr_err()` to the driver.

## Step 12 — finish init, confirm it in the log

```
finish
```
```
Value returned is $5 = 0
```
```
lx-dmesg
```

Expect this module's own `pr_info(DRIVER_NAME ": init: completed in ...
us\n")` line at the very end of the ring buffer.

## Step 13 — catch the workqueue callback firing on its own

This is the technique modules 01/02 never needed. `gpioctrl_work_fn()`
isn't called by anything you're about to type — `schedule_delayed_work()`
armed it during init, on a `poll_ms` timer (default 500ms). Set the
breakpoint and `continue`; do nothing else in `vmb` and it still fires:

```
break gpioctrl_work_fn
```
```
continue
```

Within roughly half a second:

```
Thread 2 hit Breakpoint N, gpioctrl_work_fn (work=0x...) at gpioctrl.c:324
```
```
bt
```
```
#0  gpioctrl_work_fn (work=0x...) at gpioctrl.c:324
#1  0x... in process_one_work (worker=0x..., work=0x...) at kernel/workqueue.c:...
#2  0x... in worker_thread (...) at kernel/workqueue.c:...
#3  0x... in kthread (...) at kernel/kthread.c:...
```

**Read this backtrace carefully — it's the whole point of this module.**
No `insmod`, `rmmod`, or userspace syscall anywhere in this stack. The
bottom frames are generic kernel workqueue machinery — a dedicated
kernel thread pulling queued work items and running them. Confirm which
thread:

```
lx-ps
```

Find the line whose pid matches `$lx_current()->pid` — its `comm` field
will be something like `kworker/0:2`, never `insmod` or a shell.

```
next
```
```
print ret
```
```
next
```
```
print delay
```
```
finish
```

Every time you `continue` again from here, you'll hit the same
breakpoint on the next tick — this driver's poll loop, live, one
iteration per stop, for as long as you keep continuing.

## Step 14 — the shared core: `gpioctrl_sample_once`

Both the workqueue path (step 13) and a manual `/dev/gpioctrl` write
funnel through this one function — the mutex-protected read-modify-write
at the heart of this driver (the same pattern
[11_concurrency_locking](../11_concurrency_locking/) dedicates a whole
module to, seen here in a real driver instead of an artificial test):

```
break gpioctrl_sample_once
```
```
continue
```

Let it hit from the workqueue (no action needed):

```
next
```
```
print button_value
```
```
next
```
```
print state->button
```

The *previous* sample, still in state at this point — `mutex_lock()` has
just been stepped over.

```
next
```
```
print desired_led
```
```
print changed
```

`changed` is only `true` if `button_value` differs from the previous
`state->button`. Toggle the simulated button between two `continue`s to
force that:

**Pane: vmb**

```bash
echo pull-up | tee /sys/devices/platform/gpio-sim.0/*/sim_gpio20/pull
```

The `*` matters and is not a placeholder — per `gpio_sim_setup_sysfs()`
in `drivers/gpio/gpio-sim.c`, the `sim_gpio20`/`pull` attribute group
attaches to `chip->dev`, a child device found dynamically via
`device_find_child()`, not to the `gpio-sim.0` platform device directly
— so its exact subdirectory name isn't fixed by the driver and a shell
glob is the robust way to reach it, not a guess at what it's called.
(`gpiodetect` isn't in this minimal busybox build, so this is the
practical way in without it; list `/sys/devices/platform/gpio-sim.0/`
yourself if you want to see the real name the glob matched.)

**Pane: gdb**

```
continue
```

`changed = true` on this hit is what makes the LED output line
(`gpiod_set_value_cansleep`) actually execute instead of being skipped —
the driver only touches the output "when required."

## Step 15 — the `/dev/gpioctrl` write path

```
break gpioctrl_write
```
```
continue
```

**Pane: vmb**

```bash
echo "poll_ms=100" > /dev/gpioctrl
```

**Pane: gdb**

```
Thread 2 hit Breakpoint N, gpioctrl_write (...) at gpioctrl.c:856
```
```
next
```
```
print kbuf
```
```
next
```
```
print cmd
```
```
$6 = 0x... "poll_ms=100"
```

`cmd` now holds a real, null-terminated kernel string you can `print`
directly — `copy_from_user()` already moved it across the user/kernel
boundary by this point, exactly why breaking any earlier would show
garbage or uninitialized stack instead.

## Step 16 — clean up: the `__exit` relocation gotcha, again

`gpioctrl_exit` is `__exit`, placed in `.exit.text`, which `lx-symbols`
never relocates — same underlying cause documented in
[02_better_hello's walkthrough](../02_better_hello/gdb_walkthrough.md#step-11--the-exit-path-where-it-actually-differs-from-module-01).
`break gpioctrl_exit` right now would accept with no error but never
fire; `rmmod` would complete underneath it while GDB sits at
`Continuing.` forever.

```
delete
```
```
y
```
```
break __do_sys_delete_module
```
```
continue
```

**Pane: vmb**

```bash
rmmod gpioctrl
```

**Pane: gdb**

```
Thread N hit Breakpoint N, __do_sys_delete_module (...) at kernel/module/main.c:808
```
```
advance kernel/module/main.c:863
```
```
863         mod->exit();
```
```
print mod->exit
```
```
$7 = (void (*)(void)) 0xffff80007c32c2e0
```

(That exact address is from one real run — module memory is placed
fresh each boot even with `nokaslr`. Use whatever `print mod->exit`
gives you next.)

```
add-symbol-file /home/adiopocere/Desktop/codes/linux-kernel-project/03_gpio_sim/gpioctrl.ko -s .exit.text 0xffff80007c32c2e0
```
```
y
```
```
break gpioctrl_exit
```
```
Breakpoint N at 0x100: gpioctrl_exit. (2 locations)
```

Two locations — `N.1` is still the old broken raw-offset one, `N.2` is
the newly-relocated real one:

```
disable N.1
```
```
continue
```

**Pane: vmb**

```bash
rmmod gpioctrl
```

**Pane: gdb**

```
Thread N hit Breakpoint N.2, 0x... in cleanup_module ()
```

Reports as `cleanup_module`, not `gpioctrl_exit`, with no source line
attached — confirmed live for this exact fix pattern (module 02's
walkthrough, step 10): `add-symbol-file -s <section> <addr>` supplies an
address, not a recompiled type or line table, so GDB falls back to
whichever alias name it already knew and can't attach a line number.
`module_exit()` aliases to the legacy `cleanup_module` name, the same
mirror-image already established for the load path.

```
next
```

Steps over `cancel_delayed_work_sync()` — the call that blocks until any
*currently running* `gpioctrl_work_fn()` has finished. If `rmmod`
proceeded straight to `kfree(state)` while a workqueue callback was
mid-flight, that callback would dereference already-freed memory the
instant it resumed. Continuing:

```
next
```

`sysfs_remove_group()` — userspace can no longer reach `state`'s sysfs
files.

```
next
```

`misc_deregister()` — `/dev/gpioctrl` is gone.

```
print state->samples
```

Still readable here — `state` itself isn't freed until `kfree()` near
the very end of the function, after all three calls above.

```
finish
```

## Step 17 — clean up

**Pane: gdb**

```
delete
```
```
y
```

**Pane: vmb**

```bash
echo 0 > /sys/kernel/config/gpio-sim/gpio-device/live
```
```bash
rmdir /sys/kernel/config/gpio-sim/gpio-device/node0
```
```bash
rmdir /sys/kernel/config/gpio-sim/gpio-device
```
```bash
poweroff -f
```

**Pane: gdb**

```
quit
```

---

## What this proves

- Most of a real driver's interesting behavior does not run
  synchronously underneath `insmod`/`rmmod`/a syscall you typed — it
  runs later, on someone else's schedule (a workqueue here; a timer in
  14, a dedicated kthread in 15). A breakpoint doesn't care which: set
  it, `continue`, and it fires whenever that code path actually runs,
  identified afterward by backtrace and `lx-ps`, not by anything you did
  to trigger it (steps 13–14).
- `cancel_delayed_work_sync()` in the exit path (step 16) is the same
  lesson from the other direction — correct module unload has to
  actively wait out exactly this kind of asynchronous execution before
  freeing anything it might still touch. Deliberately reordering
  `kfree(state)` before `cancel_delayed_work_sync()` in the source and
  reasoning through what a still-running `kworker` would dereference is
  the exact use-after-free shape this call ordering prevents.
- The `.exit.text` relocation gotcha from module 02 is not specific to
  that module — every `__exit`-marked function in this repo hits it the
  same way, and the same `add-symbol-file` fix applies identically
  (step 16).
