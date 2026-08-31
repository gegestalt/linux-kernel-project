# 15 — kthreads

A dedicated kernel thread that produces data on its own schedule, and
shuts down cooperatively — `kthread_run()`, `kthread_should_stop()`, and
`kthread_stop()`, the standard lifecycle for background work that needs a
real, schedulable thread rather than a timer or workqueue.

## What this demonstrates

- **`kthread_run(fn, data, name)`** — creates *and starts* a new kernel
  thread running `fn`, named (visible in `ps`/`top` as `name`). Returns a
  `struct task_struct *`, checked with `IS_ERR()` just like any other
  fallible kernel allocation-shaped call.
- **`kthread_should_stop()`** — the cooperative shutdown signal. The
  thread's main loop checks it every iteration; nothing forces the thread
  to stop, it has to notice and exit on its own. This is why the loop body
  never runs forever unconditionally, and why a hostile or buggy thread
  function that ignores this check would make `kthread_stop()` hang.
- **`kthread_stop()`** — sets the stop flag, wakes the thread if it's
  sleeping, and *blocks the caller* until the thread function actually
  returns. By the time it returns, the thread is provably gone — contrast
  with just setting a flag and hoping, which has no such guarantee.
- **A thread genuinely sleeping in process context** —
  `msleep_interruptible()` between produced items, something lab
  [14](../14_timers_workqueues/)'s timer callback is not allowed to do.
  This is what makes `kthread_stop()` return *promptly* rather than
  waiting out however much of the sleep interval remained: waking the
  task is exactly what lets it notice `kthread_should_stop()` immediately
  instead of on the next natural loop iteration.
- **Starting and stopping the thread without unloading the module** — a
  sysfs `control` attribute (`start`/`stop`) drives `kthread_run()`/
  `kthread_stop()` interactively, so you can watch the producer's PID
  change across restarts and confirm the module survives a stop/start
  cycle cleanly.
- A deliberately simple, **non-blocking** `/dev/kthread_demo` (drains
  whatever's buffered on each fresh read, returns 0 if nothing's arrived)
  — this lab is about the producer's thread lifecycle, not consumer-side
  blocking, which lab [12](../12_wait_queues_blocking/) already covers.

## Files

| File | Purpose |
|---|---|
| `kthreads.c` | The module: producer kthread, ring buffer, `/dev/kthread_demo`, sysfs `status`/`control`. |
| `Makefile` | Build, `clean`, `check`/`checkpatch`. |

## Build

```bash
cd 15_kthreads
make
```

## Load and test

```bash
sudo insmod ./kthreads.ko
dmesg | tail -3
cat /sys/kernel/kthreads_demo/status
# running=1
# pid=<some pid>
# produced=0
# dropped=0
# buffered=0
```

Confirm the thread is a real, visible task:

```bash
PID=$(grep -oP 'pid=\K[0-9]+' /sys/kernel/kthreads_demo/status)
ps -p "$PID" -o pid,comm,stat
```

Watch it produce, and drain it:

```bash
sleep 2
cat /sys/kernel/kthreads_demo/status   # produced has moved
cat /dev/kthread_demo                   # drains everything buffered so far
cat /sys/kernel/kthreads_demo/status   # buffered=0 again
```

Stop and restart it live, watching the PID change:

```bash
echo stop | sudo tee /sys/kernel/kthreads_demo/control
cat /sys/kernel/kthreads_demo/status   # running=0 pid=-1
sleep 2
cat /dev/kthread_demo                   # nothing new arrived while stopped
echo start | sudo tee /sys/kernel/kthreads_demo/control
cat /sys/kernel/kthreads_demo/status   # running=1, a *different* pid
```

Double-start/double-stop are rejected cleanly:

```bash
echo start | sudo tee /sys/kernel/kthreads_demo/control
# tee: ...: File exists
echo stop | sudo tee /sys/kernel/kthreads_demo/control
echo stop | sudo tee /sys/kernel/kthreads_demo/control
# tee: ...: No such file or directory
```

Overrun the ring buffer (16 slots) by not reading for longer than
16 × `interval_ms`, and watch `dropped` move:

```bash
echo start | sudo tee /sys/kernel/kthreads_demo/control > /dev/null
echo 100 | sudo tee /sys/module/kthreads/parameters/interval_ms
sleep 3
cat /sys/kernel/kthreads_demo/status   # dropped > 0: buffer overwrote unread items
```

## checkpatch

```bash
make check
```

## Cleanup

```bash
sudo rmmod kthreads
dmesg | tail -3     # confirms the producer thread's own "stopping" log ran
make clean
```

## Things to try

- `rmmod` the module while the producer is mid-sleep (the default
  `interval_ms=500` gives you a wide window) and time how long `rmmod`
  takes to return — it should be near-instant, because
  `msleep_interruptible()` + `kthread_should_stop()` means
  `kthread_stop()` doesn't have to wait out the sleep.
- Change `msleep_interruptible()` to plain `msleep()` (not
  interruptible) and rebuild. `rmmod` still works, but now measurably
  waits up to the remaining sleep duration before the thread notices it
  should stop — a direct, timed demonstration of why the interruptible
  variant is the right default for a loop that needs to shut down
  promptly.
- Compare this producer's shape against lab [14](../14_timers_workqueues/)'s
  workqueue heartbeat: both run in process context and can both sleep, so
  when would you actually reach for a dedicated `kthread_run()` thread
  instead of `schedule_delayed_work()`? (Hint: think about what happens
  under memory pressure or when the shared workqueue is busy with other
  work — a dedicated kthread has its own scheduling identity, a shared
  workqueue's work items compete with everyone else's.)

## Debugging with GDB

For a full, self-contained, step-by-step session for this lab — tmux
pane layout, every command, every output explained — see
[`GDB_WALKTHROUGH.md`](GDB_WALKTHROUGH.md).

Setup: [`../GDB_DEBUGGING.md`](../GDB_DEBUGGING.md).

```gdb
(gdb) lx-symbols /home/adiopocere/Desktop/codes/linux-kernel-project
(gdb) lx-ps                     # find "kthread_demo_producer" and its pid before you break anything
(gdb) break producer_thread_fn
(gdb) continue
```

At the breakpoint: `print current->pid` should match what `lx-ps`
already showed you, and `print current->comm` reads
`kthread_demo_producer` — a real, schedulable task, unlike lab 14's
timer callback. `next` through one loop iteration to watch
`ring[ring_head]` get written and `ring_head`/`ring_count` update, then
`step` into `msleep_interruptible()` (it genuinely descends into
scheduler code, confirming this thread can sleep) and `lx-ps` again
while it's asleep in there — you should see its state change from
running to interruptible sleep.

To watch the cooperative-shutdown path specifically:
`break kthread_should_stop` (a tiny inline-ish helper, but resolvable
once `lx-symbols` has loaded), `continue`, then from the guest
`echo stop | sudo tee /sys/kernel/kthreads_demo/control` — you'll land
here on the very next loop check after `kthread_stop()` wakes the
thread, before it actually returns from `producer_thread_fn()`.

**`start_producer`/`stop_producer`/`control_store`/init/exit**, all
verified:

```bash
$ gdb -q -batch -nx -ex "file kthreads.ko" \
    -ex "info line start_producer" -ex "info line stop_producer" \
    -ex "info line control_store" -ex "info line kthreads_init" \
    -ex "info line kthreads_exit" kthreads.ko
Line 100 ... <start_producer> ...
Line 122 ... <stop_producer> ...
Line 229 ... <control_store> ...
Line 270 ... <kthreads_init> ...
Line 306 ... <kthreads_exit> ...
```

```gdb
(gdb) break start_producer
(gdb) continue
```
```bash
echo start | sudo tee /sys/kernel/kthreads_demo/control
```
```gdb
(gdb) print producer_task          # NULL - about to be created
(gdb) next                          # kthread_run() itself
(gdb) print producer_task            # a real struct task_struct * now
(gdb) print producer_task->pid         # matches what /sys/kernel/kthreads_demo/status reports next
(gdb) finish
```

`break stop_producer`, trigger with `echo stop | sudo tee
/sys/kernel/kthreads_demo/control`, and step through: `producer_task` is
copied to a local and the global set to `NULL` **before**
`kthread_stop()` is even called — `print producer_task` right after that
`next` already shows `NULL`, even though the thread is still running and
will be for a few more lines. Worth pausing on: this ordering means a
second `stop` request racing in couldn't get a stale pointer,
independent of anything `kthread_stop()` itself does.

