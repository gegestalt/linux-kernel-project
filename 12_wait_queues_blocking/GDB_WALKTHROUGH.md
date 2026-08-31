# GDB walkthrough — 12_wait_queues_blocking

`wait_queues_blocking.c` pairs a kernel timer (softirq context,
producing an "event" every `interval_ms`) with a blocking `read()`
(`wait_event_interruptible()`) and a `poll()` callback. This is the
first lab whose central object is a **task that isn't running** — a
reader blocked in `bq_read()` has voluntarily taken itself off the CPU
entirely, parked on `event_wq` until something wakes it. KGDB's
break-in freezes every CPU regardless of what's running on it, which
means it can catch a *sleeping* task exactly as easily as a running
one — this walkthrough is built around seeing what that actually looks
like from inside GDB, since it looks different from every earlier
lab's breakpoint hits.

## Environment

```bash
cd 12_wait_queues_blocking
make -C /home/adiopocere/Desktop/codes/linux_mainline M=$(pwd) modules
modinfo wait_queues_blocking.ko | grep vermagic
sudo mount -o loop /home/adiopocere/Desktop/codes/qemu-vmb/labs-disk.img /tmp/vmb-mnt
sudo mkdir -p /tmp/vmb-mnt/12_wait_queues_blocking
sudo cp wait_queues_blocking.ko /tmp/vmb-mnt/12_wait_queues_blocking/
sudo umount /tmp/vmb-mnt
```

## tmux layout

Standard `vmb` + `gdbsess` — [`../GDB_DEBUGGING.md`](../GDB_DEBUGGING.md).
You'll want a **third** way to interact with the guest for step 2 — a
second shell inside the guest is easiest if your busybox setup
supports it (a second `getty`, or just running the blocking `cat` with
`&` and continuing to use the same shell for everything else).

## Real, verified breakpoint targets

```
Line 50:  producer_fn      (the timer callback - softirq context)
Line 76:  bq_read           (the blocking read)
Line 114: bq_poll            (the poll() callback)
Line 162: trigger_store       (manual sysfs trigger)
```

## The walkthrough

### Step 1 — the producer: a timer callback, caught mid-softirq

```gdb
(gdb) target remote :1234
(gdb) lx-version
(gdb) break do_init_module
(gdb) continue
```
```bash
# vmb:
insmod /mnt/labs/12_wait_queues_blocking/wait_queues_blocking.ko interval_ms=1500
```
```gdb
(gdb) lx-symbols /home/adiopocere/Desktop/codes/linux-kernel-project
(gdb) break producer_fn
(gdb) continue
```

Don't touch `vmb` — `producer_fn` fires on its own, every
`interval_ms`, exactly like lab 03's workqueue and lab 14's timer:

```gdb
Thread 2 hit Breakpoint N, producer_fn (t=0x...) at wait_queues_blocking.c:50
(gdb) bt
#0  producer_fn (t=0x...) at wait_queues_blocking.c:50
#1  0x... in call_timer_fn (...) at kernel/time/timer.c:...
#2  0x... in __run_timers (...) at kernel/time/timer.c:...
#3  0x... in run_timer_softirq (...) at kernel/time/timer.c:...
#4  0x... in __do_softirq (...) at kernel/softirq.c:...
```

This backtrace is the concrete evidence behind the source comment
("Timer callbacks run in softirq context: no sleeping, no blocking
allocations"): the bottom frames are softirq machinery, not a task's
own thread. Confirm `current` here is whatever happened to be running
when the softirq fired, not anything meaningfully related to this
timer:

```gdb
(gdb) print current->comm
(gdb) print current->pid
```

Step through the actual work:

```gdb
(gdb) next    # atomic64_inc(&event_id)
(gdb) print event_id
(gdb) next     # atomic_set(&data_ready, 1)
(gdb) next      # wake_up_interruptible(&event_wq) - about to matter a lot in step 2
(gdb) next       # mod_timer() reschedules itself for the next interval
```

### Step 2 — catch a task actually blocked on the wait queue

This is the step this lab exists for. Delete the timer breakpoint (you
don't want it firing repeatedly while you set this up), and this time
break where a *reader* actually goes to sleep:

```gdb
(gdb) delete
(gdb) break bq_read
(gdb) continue
```
```bash
# vmb:
cat /dev/blocking_demo &
```

The first hit is the call itself, before it's decided whether to
block:

```gdb
Thread 2 hit Breakpoint N, bq_read (...) at wait_queues_blocking.c:76
(gdb) next    # atomic_xchg(&data_ready, 0) - probably 0 right now, no event pending yet
(gdb) next     # not O_NONBLOCK, so: atomic_inc(&waiter_count)
(gdb) print waiter_count
$1 = {counter = 1}
(gdb) next      # wait_event_interruptible(event_wq, atomic_read(&data_ready))
```

**This next is the one to watch closely.** `wait_event_interruptible()`
is a macro that, if the condition is false, actually calls `schedule()`
— this task genuinely leaves the CPU here. Depending on exact timing
GDB may show the `next` "completing" only once the condition becomes
true (because a real reschedule happened underneath it) — if it seems
to hang, that's not a bug, it's this task legitimately asleep;
`continue` instead of `next` if you want to let time pass normally
while it waits (remembering the guest is otherwise frozen the whole
time GDB is stopped anywhere — real wall-clock time inside the guest
only advances between your `continue`s, not while you're reading
output at a breakpoint).

Confirm the sleeping task's own state directly, without single-stepping
further — `lx-ps` shows every task's state field:

```gdb
(gdb) lx-ps
```

Find the `cat` process in the listing — its state should read
something like `INTERRUPTIBLE` (kernel `TASK_INTERRUPTIBLE`), not
`RUNNING`. Compare this against `producer_fn`'s task in step 1, or
against `insmod`/`rmmod` in any earlier lab — those were always
`RUNNING` (or briefly preempted, but never voluntarily parked) at the
moment you caught them, because a syscall's own callback code was
still actively executing. This `cat` is different: it has explicitly
told the scheduler "wake me only when this condition is true," and
handed control away in the meantime.

### Step 3 — wake it up, and watch the *same* stopped `bq_read` resume

With the reader still parked from step 2, trigger an event from a
second path — the manual sysfs trigger rather than waiting out the
timer, so you control exactly when this happens:

```bash
# vmb, a second shell/session:
echo 1 | tee /sys/class/misc/blocking_demo/trigger
```

(`bq_attr_group` is attached to `bq_miscdev.this_device->kobj` in the
source — the misc device's own kobject — which is why this lives under
`/sys/class/misc/blocking_demo/`, the same location pattern lab 11's
`/sys/class/misc/race_demo/mode` uses, rather than under
`/sys/kernel/...` the way labs 13–16's bare-kobject drivers do.)

```gdb
(gdb) continue
```

The very same `bq_read` call from step 2 — not a new one — now
resumes past the `wait_event_interruptible()` call, because
`wake_up_interruptible(&event_wq)` (from `trigger_store`, or from the
next `producer_fn` tick if you didn't beat the timer to it) made its
condition true:

```gdb
Thread 2 hit Breakpoint N, bq_read (...) at wait_queues_blocking.c:76
(gdb) print waiter_count
$2 = {counter = 0}    # atomic_dec_return already ran - back to 0
```

Wait — if you land back at the top of `bq_read`'s `for (;;)` loop, this
is correct: `wait_event_interruptible()` returning doesn't mean you're
past the function, it means the loop's condition check runs again,
this time finding `atomic_xchg(&data_ready, 0)` true and `break`ing out
for real:

```gdb
(gdb) next
(gdb) print id
```

### Step 4 — `poll()`: readiness without blocking

```gdb
(gdb) delete
(gdb) break bq_poll
(gdb) continue
```

Any tool that uses `select()`/`poll()`/`epoll()` against
`/dev/blocking_demo` triggers this — a plain `cat` never will, since
`cat` just calls `read()` directly. If your busybox build lacks a
convenient poll-driving tool, the mechanics are still worth reading
statically:

```gdb
(gdb) next    # poll_wait(file, &event_wq, wait) - registers interest, does NOT block
(gdb) next     # atomic_read(&data_ready) check
(gdb) finish
```

The key structural point (worth understanding even without a live hit):
`poll_wait()` itself never sleeps — it just tells the *caller*
(`select()`/`poll()`'s own generic kernel code) which wait queue to
watch. The actual blocking, if any, happens one layer up, potentially
across many file descriptors from many different drivers at once,
which is precisely why this callback has to be non-blocking itself.

## Cleanup

```gdb
(gdb) delete
(gdb) break wait_queues_blocking_exit
(gdb) continue
```
```bash
# vmb:
rmmod wait_queues_blocking
```
```gdb
Thread 2 hit Breakpoint N, wait_queues_blocking_exit () at wait_queues_blocking.c:222
(gdb) next   # timer_shutdown_sync(&producer_timer)
```

Read the source's own comment on this line before stepping past it:
it explains that `rmmod` can never actually reach here while a reader
is genuinely blocked in `bq_read()`, because `fops.owner =
THIS_MODULE` means every open fd holds a module reference for its
whole lifetime, and `rmmod` refuses to unload a module with a nonzero
reference count. If you still have a backgrounded `cat` from step 2/3
holding the device open, `rmmod` will simply fail with "in use" rather
than this exit path ever running concurrently with a blocked reader —
confirm it yourself: `kill` the backgrounded `cat` first, *then*
`rmmod`.

```bash
# vmb:
poweroff -f
```

## What this proves

A blocked `read()` is not "still running, just slow" — `lx-ps` showing
`TASK_INTERRUPTIBLE` instead of `RUNNING` is direct evidence the
scheduler has genuinely removed this task from the CPU, and `continue`
from a breakpoint set *before* the block can carry you across a real
sleep/wake cycle spanning wall-clock time and a completely separate
trigger, landing back in the same function with the same local
variables it had before, resumed rather than restarted. Contrast this
against every synchronous callback in labs 01–11: those never left the
CPU at all between entry and return.
