# GDB walkthrough — 14_timers_workqueues, hands-on, start to finish

`timers_workqueues.c` runs two independent "heartbeats" side by side —
a `struct timer_list` and a `struct delayed_work` — doing the exact
same job (tick a counter, record the execution context) so the only
real variable on display is *where* the kernel actually ran each one.
This walkthrough makes that difference undeniable: breaking on both
callbacks and reading `in_softirq()`/`in_task()`/`preemptible()` directly
out of the frozen kernel's own state at each stop, rather than trusting
the source comment's claim about which context each one runs in.

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

*Regular terminal (detach with `Ctrl-b d`, or a separate window).*

```bash
cd /home/adiopocere/Desktop/codes/linux-kernel-project/14_timers_workqueues
make -C /home/adiopocere/Desktop/codes/linux_mainline M=$(pwd) modules
```

## Step 2 — check vermagic, copy onto the scratch disk

```bash
modinfo timers_workqueues.ko | grep vermagic
```
```bash
sudo mount -o loop /home/adiopocere/Desktop/codes/qemu-vmb/labs-disk.img /tmp/vmb-mnt
sudo mkdir -p /tmp/vmb-mnt/14_timers_workqueues
sudo cp timers_workqueues.ko /tmp/vmb-mnt/14_timers_workqueues/
sudo umount /tmp/vmb-mnt
```

## Step 3 — boot the guest

**Pane: vmb**

```bash
qemu-system-aarch64 -M virt -cpu max -m 1024 -smp 2 \
  -kernel /home/adiopocere/Desktop/codes/linux_mainline/arch/arm64/boot/Image \
  -initrd /home/adiopocere/Desktop/codes/qemu-vmb/initramfs.cpio.gz \
  -drive file=/home/adiopocere/Desktop/codes/qemu-vmb/labs-disk.img,if=virtio,format=raw \
  -append "console=ttyAMA0 rdinit=/init nokaslr" -nographic -s
```

Wait for `=== VM B (QEMU) ready ===` and `~ #`.

## Step 4 — start gdb, connect

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

## Step 5 — break on the load entry point

**Pane: gdb**

```
break do_init_module
```
```
continue
```

Prints `Continuing.` — switch panes.

## Step 6 — load with both intervals slow enough to catch by hand

**Pane: vmb**

```bash
insmod /mnt/labs/14_timers_workqueues/timers_workqueues.ko timer_interval_ms=3000 work_interval_ms=3000
```

**Pane: gdb**

```
Thread 2 hit Breakpoint 1, do_init_module (mod=mod@entry=0x...) at kernel/module/main.c:3089
```
```
lx-symbols /home/adiopocere/Desktop/codes/linux-kernel-project
```

## Step 7 — the timer: caught in softirq context

**Pane: gdb**

```
break heartbeat_timer_fn
```
```
continue
```

Nothing to do in `vmb` — it fires on its own within 3 seconds:

```
Thread 2 hit Breakpoint 2, heartbeat_timer_fn (t=0x...) at timers_workqueues.c:81
```
```
next
```

(`might_sleep()` — a debug assertion only, does nothing here since
nothing actually sleeps in this path.)

```
next
```

(`format_ctx(ctx, sizeof(ctx))` just ran.)

```
print ctx
```
```
$1 = "in_interrupt=0 in_softirq=1 in_task=0 preemptible=0 comm=... pid=..."
```

**What this shows:** this string is the driver's own
`in_interrupt()`/`in_softirq()`/`in_task()`/`preemptible()` calls,
evaluated *at this exact instant*, formatted into a buffer, which you
just printed back out. `in_softirq=1`, `in_task=0` is direct, first-hand
confirmation that this genuinely is softirq context — not merely
"running from something timer-related."

```
bt
```
```
#0  heartbeat_timer_fn (t=0x...) at timers_workqueues.c:81
#1  0x... in call_timer_fn (...) at kernel/time/timer.c:...
#2  0x... in __run_timers (...) at kernel/time/timer.c:...
#3  0x... in run_timer_softirq (...) at kernel/time/timer.c:...
#4  0x... in __do_softirq (...) at kernel/softirq.c:...
```

```
print current->comm
```

**What this shows:** `current` here is genuinely whatever task happened
to be executing when the softirq fired — not a dedicated thread the way
module 15's kthread is. `continue` once more (wait ~3s) and re-run this
same `print current->comm`: you will very likely see a *different*
`comm`/`pid` each time, because `current` is arbitrary here, not tied to
this timer in any way.

```
next
```

(`spin_lock(&stats_lock)`.)

```
next
```

(`timer_ticks++`.)

```
print timer_ticks
```
```
next
```

(`strscpy`, `spin_unlock`.)

```
next
```

(`mod_timer()` reschedules itself.)

```
finish
```

## Step 8 — the workqueue: the same job, a genuinely different context

**Pane: gdb**

```
delete
```
```
y
```

(Bare `delete` with no argument deletes *every* breakpoint, but first
asks `Delete all breakpoints? (y or n)` — naming a specific number
instead skips the prompt if you'd rather keep others armed.)

```
break heartbeat_work_fn
```
```
continue
```
```
Thread 2 hit Breakpoint 3, heartbeat_work_fn (work=0x...) at timers_workqueues.c:103
```
```
bt
```
```
#0  heartbeat_work_fn (work=0x...) at timers_workqueues.c:103
#1  0x... in process_one_work (...) at kernel/workqueue.c:...
#2  0x... in worker_thread (...) at kernel/workqueue.c:...
#3  0x... in kthread (...) at kernel/kthread.c:...
```

**What this shows:** completely different bottom frames from step 7 —
`process_one_work`/`worker_thread`/`kthread`, the same shape module 03's
workqueue callback showed.

```
print current->comm
```
```
$2 = "kworker/0:1\000..."
```

**What this shows:** `current` here is a real, dedicated `kworker` task
— not arbitrary, unlike step 7.

```
next
```

(`usleep_range(1000, 2000)` — a genuine sleep.)

**What this shows:** this `next` takes real wall-clock time to
complete — somewhere between 1 and 2 milliseconds of actual scheduling,
not an instant step, because `usleep_range()` really does put this task
to sleep and let the scheduler run something else meanwhile. Nothing
comparable happened at the equivalent point in step 7, because nothing
in `heartbeat_timer_fn` ever sleeps.

```
next
```

(`format_ctx()` just ran.)

```
print ctx
```
```
$3 = "in_interrupt=0 in_softirq=0 in_task=1 preemptible=1 comm=kworker/0:1 pid=..."
```

**What this shows:** directly contrast this string against step 7's.
`in_task=1`, `in_softirq=0`, `preemptible=1` — the complete opposite
pattern from the timer callback, for what is, source-code-wise, an
almost identical function. This pair of `format_ctx()` outputs is the
entire module, distilled to two strings read straight out of memory.

```
next
```

(`spin_lock`, `work_ticks++`, `strscpy`, `spin_unlock`.)

```
next
```

(`schedule_delayed_work()` reschedules itself.)

```
finish
```

## Step 9 — read both stats side by side

**Pane: gdb**

```
delete
```
```
y
```

**Pane: vmb**

```bash
cat /sys/kernel/timers_workqueues/stats
```
```
timer_ticks=N
timer_last_ctx: in_interrupt=0 in_softirq=1 in_task=0 preemptible=0 comm=... pid=...
work_ticks=M
work_last_ctx: in_interrupt=0 in_softirq=0 in_task=1 preemptible=1 comm=kworker/... pid=...
```

**What this shows:** `timer_last_ctx`/`work_last_ctx` are exactly the
same strings you already inspected live in GDB, persisted into module
state by each callback's own `strscpy()` — this file is reading the
*result* of the same mechanism you just watched build that result, one
field write at a time.

## Step 10 — the exit path: `__exit`-section relocation gotcha

`timers_workqueues_exit` is marked `__exit`, placing it in its own ELF
section, `.exit.text`, which `lx-symbols` never relocates (its hardcoded
section list in `scripts/gdb/linux/symbols.py` doesn't include
`.init.text`/`.exit.text`). A direct `break timers_workqueues_exit` right
now would silently resolve to a raw, unrelocated file offset — no error,
it just never fires. This affects every module in this repo using the
modern `module_exit()` macro (every module except 01).

**Pane: gdb**

```
break __do_sys_delete_module
```
```
continue
```

**Pane: vmb**

```bash
rmmod timers_workqueues
```

**Pane: gdb**

```
advance kernel/module/main.c:863
```
```
print mod->exit
```
```
$4 = (void (*)(void)) 0xffff80007c320580
```

(That address is from one real run and won't match yours — module
memory placement is random per boot regardless of `nokaslr`. Always use
whatever `print mod->exit` gives you right now. **Do not `step` into it
from here** — with no relocated line table GDB can't bound the function
and `step` free-runs straight past it; `Ctrl-C` recovers you.)

```
add-symbol-file /home/adiopocere/Desktop/codes/linux-kernel-project/14_timers_workqueues/timers_workqueues.ko -s .exit.text 0xffff80007c320580
```
```
y
```
```
break timers_workqueues_exit
```
```
Breakpoint 5 at 0xffff80007c320580: file timers_workqueues.c, line 195.
```
```
delete
```
```
y
```
```
continue
```

**Pane: vmb**

```bash
rmmod timers_workqueues
```

**Pane: gdb**

```
Thread 2 hit Breakpoint 5, timers_workqueues_exit () at timers_workqueues.c:195
195		timer_shutdown_sync(&heartbeat_timer);
```
```
next
```

(`timer_shutdown_sync(&heartbeat_timer)`.)

```
next
```

(`cancel_delayed_work_sync(&heartbeat_work)`.)

**What this shows:** both of these block until any in-flight callback
has actually finished — the same guarantee module 03's `gpioctrl_exit()`
and module 12's exit path each depend on. "Something might still be
running asynchronously" is a recurring fact of real driver code, not a
one-off concern specific to any single module.

```
continue
```

## Step 11 — clean up

**Pane: vmb**

```bash
poweroff -f
```

**Pane: gdb**

```
quit
```

---

## What this proves

`in_softirq()`/`in_task()`/`preemptible()` aren't documentation-only
concepts — they're real predicates you can call (or, as here, read the
already-computed results of) at any breakpoint, and two structurally
similar callbacks genuinely do report opposite answers depending on
what invoked them (steps 7 and 8). The concrete, visible consequence:
one context can call `usleep_range()` and pay real wall-clock time doing
it; the other cannot call anything that sleeps at all, on pain of a
`might_sleep()` warning (or worse, on a kernel without the debug check
compiled in) — a distinction every timer/workqueue/tasklet/
interrupt-handler author in the kernel has to track correctly, made
directly observable here.
