# 14 — timers_workqueues

A `struct timer_list` and a `struct delayed_work`, doing the exact same
job (tick a counter, reschedule itself) forever, side by side — so the
only thing left to actually compare is *where the kernel runs each one*.

## What this demonstrates

- **`struct timer_list` runs in softirq context** (`run_timer_softirq()`).
  No sleeping, no blocking allocation. `current` inside the callback is
  whatever task happened to be executing when the softirq fired — not a
  thread "belonging" to the timer. Labs [03](../03_gpio_sim/) and
  [12](../12_wait_queues_blocking/) already used a timer/delayed-work
  callback each without dwelling on this; this lab makes the context
  itself the thing under test.
- **`struct delayed_work` runs in process context**, on a real kworker
  thread. It can sleep, allocate with `GFP_KERNEL`, take a mutex — none
  of which the timer callback can safely do.
- **`might_sleep()`** — the kernel's own debug assertion for calling a
  sleeping function from a context that forbids it. Called (harmlessly)
  from the timer callback here: on a kernel built with
  `CONFIG_DEBUG_ATOMIC_SLEEP=y` it prints a non-fatal "sleeping function
  called from invalid context" warning; without that debug option, it's a
  silent no-op either way — check which kind of kernel you're running
  with `grep CONFIG_DEBUG_ATOMIC_SLEEP /boot/config-$(uname -r)`.
- **`usleep_range()` instead of `msleep()` for a short, real sleep** in
  the workqueue callback — proving process context genuinely can block —
  and a checkpatch-driven reason why: `msleep()` under ~20ms can actually
  sleep for up to 20ms due to timer/HZ granularity, which is why
  `checkpatch --strict` warns on `msleep()` calls under 20ms and the
  kernel provides `usleep_range()` (backed by hrtimers) for short,
  reasonably-precise sleeps instead.
- **`timer_shutdown_sync()`** (from lab [12](../12_wait_queues_blocking/))
  next to **`cancel_delayed_work_sync()`** (from lab
  [03](../03_gpio_sim/)) in the same teardown path — the timer and
  workqueue equivalents of "wait for any in-flight callback to finish and
  guarantee it won't reschedule itself," required before it's safe to
  unregister anything the callback touches.

## Files

| File | Purpose |
|---|---|
| `timers_workqueues.c` | The module: a timer heartbeat and a workqueue heartbeat, each recording its own execution-context snapshot. |
| `Makefile` | Build, `clean`, `check`/`checkpatch`. |

## Build

```bash
cd 14_timers_workqueues
make
```

## Load and test

```bash
sudo insmod ./timers_workqueues.ko
dmesg | tail -3
cat /sys/kernel/timers_workqueues/stats
```

Watch both counters advance, and compare the two context snapshots
directly:

```bash
watch -n 1 cat /sys/kernel/timers_workqueues/stats
```

Expected shape:

```
timer_ticks=12
timer_last_ctx: in_interrupt=1 in_softirq=1 in_task=0 preemptible=0 comm=<whatever task was running> pid=<its pid>
work_ticks=12
work_last_ctx: in_interrupt=0 in_softirq=0 in_task=1 preemptible=1 comm=kworker/u8:1 pid=<a real kworker pid>
```

The interesting line is `timer_last_ctx`'s `comm`/`pid`: it will jump
around between whatever process/thread happened to be running when the
softirq fired (frequently your own shell, or `swapper/N` if a CPU was
idle) — it is *not* a stable "timer thread" the way `work_last_ctx`'s
`kworker/...` consistently is.

If your kernel has `CONFIG_DEBUG_ATOMIC_SLEEP=y`, you'll also see a
one-time (or repeated, depending on kernel version) `dmesg` splat from
`might_sleep()` in the timer path shortly after load:

```bash
grep -A5 "BUG: sleeping function called" /var/log/kern.log 2>/dev/null || dmesg | grep -A5 "BUG: sleeping"
```

Change the intervals live and watch each counter's rate change
independently:

```bash
echo 100 | sudo tee /sys/module/timers_workqueues/parameters/timer_interval_ms
echo 5000 | sudo tee /sys/module/timers_workqueues/parameters/work_interval_ms
watch -n 1 cat /sys/kernel/timers_workqueues/stats
```

## checkpatch

```bash
make check
```

## Cleanup

```bash
sudo rmmod timers_workqueues
dmesg | tail -3    # final tick counts
make clean
```

## Things to try

- Set both intervals very low (`10` ms) and watch `top -H` or `ps -eLo
  pid,comm,pcpu | grep kworker` while the module is loaded — the
  workqueue heartbeat shows up as real, schedulable thread activity; the
  timer heartbeat does not appear as a thread anywhere, because it isn't
  one.
- Temporarily replace `usleep_range(1000, 2000)` in
  `heartbeat_work_fn()` with `msleep(1)`, rebuild, and run `make check`
  again to see checkpatch's `MSLEEP` warning fire — then read
  `Documentation/timers/timers-howto.rst` in `../../linux_mainline` for
  the kernel's own guidance on which sleep primitive to use at which
  duration.
- This is the one genuinely unsafe experiment worth trying *only* in your
  disposable VM: replace `might_sleep();` in `heartbeat_timer_fn()` with
  an actual `usleep_range(1000, 2000);` call (moving the workqueue's safe
  sleep into the timer's unsafe context) and rebuild. On a kernel with
  `CONFIG_DEBUG_ATOMIC_SLEEP=y` expect a loud `dmesg` splat; on one
  without it, behavior is undefined by the kernel's own contract — it may
  appear to work, or it may not, which is exactly the point: "seems to
  work in my testing" is not the same as "is correct," and this is the
  cheapest possible way to watch that gap in person instead of taking it
  on faith.

## Debugging with GDB

Setup: [`../GDB_DEBUGGING.md`](../GDB_DEBUGGING.md). Break on both
callbacks and read their execution context directly out of the running
kernel, rather than trusting the `format_ctx()` string the driver builds
for you:

```gdb
(gdb) lx-symbols /home/adiopocere/Desktop/codes/linux-kernel-project
(gdb) break heartbeat_timer_fn
(gdb) continue
```
```gdb
(gdb) finish            # let format_ctx() run, then...
(gdb) print ctx          # "in_softirq=1 in_task=0 ..." - see it for yourself, not just in sysfs
(gdb) break heartbeat_work_fn
(gdb) continue
(gdb) finish
(gdb) print ctx           # "in_softirq=0 in_task=1 comm=kworker/..."
```

Since `might_sleep()` is a debug *check*, not a breakpoint-worthy event
on its own, the more direct way to feel the context difference is
`step`-ing into `usleep_range()` from `heartbeat_work_fn` (it actually
descends into scheduler code — `bt` will show real sleep/wake frames)
versus trying the same `step` on `might_sleep()` in the timer path
(nothing to descend into on a kernel without
`CONFIG_DEBUG_ATOMIC_SLEEP`, confirming the "harmless no-op" claim from
this lab's README empirically instead of by assertion).

## Tracing this live

Setup and general method: [`../FTRACE_TRACING.md`](../FTRACE_TRACING.md).

```bash
sudo bpftrace -l 'kprobe:timers_workqueues:*'
```
```
kprobe:timers_workqueues:format_ctx.constprop.0
kprobe:timers_workqueues:heartbeat_timer_fn
kprobe:timers_workqueues:heartbeat_work_fn
kprobe:timers_workqueues:stats_show
```

```bash
sudo bpftrace -e '
kprobe:timers_workqueues:heartbeat_timer_fn { printf("TIMER  by %s[%d]\n", comm, pid); }
kprobe:timers_workqueues:heartbeat_work_fn  { printf("WORKQ  by %s[%d]\n", comm, pid); }
' &
sleep 4
```

Real captured output over ~4 seconds:

```
TIMER  by cc1[178988]
WORKQ  by kworker/2:1[41698]
TIMER  by llvmpipe-0[3814]
WORKQ  by kworker/2:1[41698]
TIMER  by llvmpipe-1[3814]
WORKQ  by kworker/2:1[41698]
TIMER  by cc1[179041]
WORKQ  by kworker/2:1[41698]
```

The whole point of this lab, visible directly: `TIMER` is a different
task almost every single time; `WORKQ` is the *exact same*
`kworker/2:1` every single time. One is "whatever happened to be
running," the other is a real, consistent, schedulable thread.

