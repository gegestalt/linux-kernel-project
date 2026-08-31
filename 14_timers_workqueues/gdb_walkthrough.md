# GDB walkthrough — 14_timers_workqueues

`timers_workqueues.c` runs two independent "heartbeats" side by side —
a `struct timer_list` and a `struct delayed_work` — doing the exact
same job (tick a counter, record the execution context) so the only
real variable on display is *where* the kernel actually ran each one.
This module's walkthrough is built to make that difference undeniable:
breaking on both callbacks and reading `in_softirq()`/`in_task()`/
`preemptible()` directly out of the frozen kernel's own state at each
stop, rather than trusting the source comment's claim about which
context each one runs in.

## Environment

```bash
cd 14_timers_workqueues
make -C /home/adiopocere/Desktop/codes/linux_mainline M=$(pwd) modules
modinfo timers_workqueues.ko | grep vermagic
sudo mount -o loop /home/adiopocere/Desktop/codes/qemu-vmb/labs-disk.img /tmp/vmb-mnt
sudo mkdir -p /tmp/vmb-mnt/14_timers_workqueues
sudo cp timers_workqueues.ko /tmp/vmb-mnt/14_timers_workqueues/
sudo umount /tmp/vmb-mnt
```

## tmux layout

Standard `vmb` + `gdbsess` — [`../gdb_debugging.md`](../gdb_debugging.md).

## Real, verified breakpoint targets

```
Line 81:  heartbeat_timer_fn   (softirq context)
Line 103: heartbeat_work_fn     (kworker process context)
Line 61:  format_ctx
Line 166: timers_workqueues_init
Line 195: timers_workqueues_exit
```

## The walkthrough

### Step 1 — load with both intervals slow enough to catch by hand

```gdb
(gdb) target remote :1234
(gdb) lx-version
(gdb) break do_init_module
(gdb) continue
```
```bash
# vmb:
insmod /mnt/labs/14_timers_workqueues/timers_workqueues.ko timer_interval_ms=3000 work_interval_ms=3000
```
```gdb
(gdb) lx-symbols /home/adiopocere/Desktop/codes/linux-kernel-project
```

### Step 2 — the timer: caught in softirq context

```gdb
(gdb) break heartbeat_timer_fn
(gdb) continue
```

Nothing to do in `vmb` — it fires on its own within 3 seconds:

```gdb
Thread 2 hit Breakpoint N, heartbeat_timer_fn (t=0x...) at timers_workqueues.c:81
(gdb) next    # might_sleep() - a debug assertion only, does nothing here since nothing actually sleeps
(gdb) next     # format_ctx(ctx, sizeof(ctx))
(gdb) print ctx
$1 = "in_interrupt=0 in_softirq=1 in_task=0 preemptible=0 comm=... pid=..."
```

Read this string carefully — it's the driver's own
`in_interrupt()`/`in_softirq()`/`in_task()`/`preemptible()` calls,
evaluated *at this exact instant*, formatted into a buffer, which you
are now printing back out. `in_softirq=1`, `in_task=0` is the direct,
first-hand confirmation of the source comment's claim: this genuinely
is softirq context, not merely "running from something timer-related."

```gdb
(gdb) bt
#0  heartbeat_timer_fn (t=0x...) at timers_workqueues.c:81
#1  0x... in call_timer_fn (...) at kernel/time/timer.c:...
#2  0x... in __run_timers (...) at kernel/time/timer.c:...
#3  0x... in run_timer_softirq (...) at kernel/time/timer.c:...
#4  0x... in __do_softirq (...) at kernel/softirq.c:...
```

`current` here is genuinely whatever task happened to be executing when
the softirq fired — confirm it isn't a dedicated thread the way module
15's kthread is:

```gdb
(gdb) print current->comm
```

Run this a second time on the next tick (`continue`, wait ~3s) — you
will very likely see a *different* `comm`/`pid` each time, because
`current` is arbitrary here, not tied to this timer in any way. This is
the concrete meaning of "current here is whatever task happened to be
running when the softirq fired, not a thread that belongs to this
timer" from the source comment.

```gdb
(gdb) next   # spin_lock(&stats_lock)
(gdb) next    # timer_ticks++
(gdb) print timer_ticks
(gdb) next     # strscpy, spin_unlock
(gdb) next      # mod_timer() reschedules itself
(gdb) finish
```

### Step 3 — the workqueue: the same job, a genuinely different context

```gdb
(gdb) delete
(gdb) break heartbeat_work_fn
(gdb) continue
```
```gdb
Thread 2 hit Breakpoint N, heartbeat_work_fn (work=0x...) at timers_workqueues.c:103
(gdb) bt
#0  heartbeat_work_fn (work=0x...) at timers_workqueues.c:103
#1  0x... in process_one_work (...) at kernel/workqueue.c:...
#2  0x... in worker_thread (...) at kernel/workqueue.c:...
#3  0x... in kthread (...) at kernel/kthread.c:...
```

Different bottom frames from step 2 entirely — `process_one_work`/
`worker_thread`/`kthread`, the same shape module 03's workqueue callback
showed. `current` here is a real, dedicated `kworker` task:

```gdb
(gdb) print current->comm
$2 = "kworker/0:1\000..."
```

Step past the one line the timer callback's comment specifically calls
out as illegal in its own context:

```gdb
(gdb) next    # usleep_range(1000, 2000) - a genuine sleep
```

**This `next` takes real wall-clock time to complete** — somewhere
between 1 and 2 milliseconds of actual scheduling, not an instant
step, because `usleep_range()` really does put this task to sleep and
let the scheduler run something else in the meantime. Nothing
comparable happened at the equivalent point in step 2, because nothing
in `heartbeat_timer_fn` ever sleeps.

```gdb
(gdb) next    # format_ctx
(gdb) print ctx
$3 = "in_interrupt=0 in_softirq=0 in_task=1 preemptible=1 comm=kworker/0:1 pid=..."
```

**Directly contrast this string against step 2's.** `in_task=1`,
`in_softirq=0`, `preemptible=1` — the complete opposite pattern from
the timer callback, for what is, source-code-wise, an almost identical
function. This pair of `format_ctx()` outputs is the entire module,
distilled to two strings you read straight out of memory.

```gdb
(gdb) next    # spin_lock, work_ticks++, strscpy, spin_unlock
(gdb) next     # schedule_delayed_work() reschedules itself
(gdb) finish
```

### Step 4 — read both stats side by side, confirm they've been ticking independently

```gdb
(gdb) delete
```
```bash
# vmb:
cat /sys/kernel/timers_workqueues/stats
```
```
timer_ticks=N
timer_last_ctx: in_interrupt=0 in_softirq=1 in_task=0 preemptible=0 comm=... pid=...
work_ticks=M
work_last_ctx: in_interrupt=0 in_softirq=0 in_task=1 preemptible=1 comm=kworker/... pid=...
```

`timer_last_ctx`/`work_last_ctx` are exactly the same strings you
already inspected live in GDB, persisted into module state by each
callback's own `strscpy()` — this file is reading the *result* of the
same mechanism you just watched build that result, one field write at
a time.

## Cleanup

```gdb
(gdb) break timers_workqueues_exit
(gdb) continue
```
```bash
# vmb:
rmmod timers_workqueues
```
```gdb
Thread 2 hit Breakpoint N, timers_workqueues_exit () at timers_workqueues.c:195
(gdb) next   # timer_shutdown_sync(&heartbeat_timer)
(gdb) next    # cancel_delayed_work_sync(&heartbeat_work)
```

Both of these block until any in-flight callback has actually
finished, the same guarantee module 03's `gpioctrl_exit()` and module
12's exit path each depend on — worth noticing this is now the *third*
module in this repo whose safe-unload logic hinges on exactly this kind of
wait, because "something might still be running asynchronously" is a
recurring fact of real driver code, not a one-off concern specific to
any single module.

```bash
# vmb:
poweroff -f
```

## What this proves

`in_softirq()`/`in_task()`/`preemptible()` aren't documentation-only
concepts — they're real predicates you can call (or, as here, read the
already-computed results of) at any breakpoint, and two structurally
similar callbacks genuinely do report opposite answers depending on
what invoked them. The concrete, visible consequence: one context can
call `usleep_range()` and pay real wall-clock time doing it; the other
cannot call anything that sleeps at all, on pain of a `might_sleep()`
warning (or worse, on a kernel without the debug check compiled in) —
a distinction every timer/workqueue/tasklet/interrupt-handler author
in the kernel has to track correctly, made directly observable here.
