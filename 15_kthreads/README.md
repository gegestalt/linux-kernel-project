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
