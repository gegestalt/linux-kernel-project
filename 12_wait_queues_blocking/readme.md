# 12 — wait_queues_blocking

A device that produces a new "event" periodically, and a `read()` that
actually blocks until one shows up — instead of every read() in this repo
so far, which has always had an answer instantly. Plus `poll()`/`select()`
support, and non-blocking (`O_NONBLOCK`) reads.

## What this demonstrates

- **`wait_event_interruptible(wq, condition)`** — puts the calling task to
  sleep until `condition` becomes true (re-checked every time `wq` is
  woken), and returns non-zero (`-ERESTARTSYS`) if a signal interrupts the
  wait instead. The `_interruptible` matters: it's what makes `cat
  /dev/blocking_demo` respond instantly to Ctrl-C instead of being stuck
  in an unkillable wait.
- **A producer waking the wait queue** — a `struct timer_list` (the
  simplest possible producer; see module [14](../14_timers_workqueues/) for
  timers on their own) fires periodically, flips a "data ready" flag, and
  calls `wake_up_interruptible(&wq)`. This is the same shape as a real
  interrupt handler waking a driver's read-side wait queue once new data
  has arrived.
- **`atomic_xchg()` to avoid a thundering-herd race** — with more than one
  reader blocked, a naive "check the flag, then clear it" done as two
  separate steps would let multiple woken readers all believe they got
  the same event. `atomic_xchg(&data_ready, 0)` makes check-and-clear one
  indivisible step, so exactly one waiting reader consumes each produced
  event.
- **`O_NONBLOCK`** — checked via `file->f_flags`, the same field decoded in
  module [08](../08_open_release_cdev/). With it set, `read()` returns
  `-EAGAIN` immediately instead of blocking when there's nothing ready —
  the flag a caller sets at `open()` time to opt out of blocking
  semantics entirely.
- **`poll()`/`select()` support** — `bq_poll()` calls `poll_wait()` to
  register this file on the wait queue (without itself blocking; the VFS
  layer is what actually sleeps, potentially across many file descriptors
  from one `select()` call) and reports current readiness via
  `EPOLLIN`/`EPOLLRDNORM`.
- **`timer_shutdown_sync()`** on module unload — waits for any in-flight
  timer callback to finish and guarantees it won't re-arm itself, which
  matters because `producer_fn()` calls `mod_timer()` on itself; getting
  the shutdown-vs-still-rearming race wrong here is a classic
  use-after-free.

## Files

| File | Purpose |
|---|---|
| `wait_queues_blocking.c` | The driver: `/dev/blocking_demo` (blocking/non-blocking read + poll) and sysfs `event_id`/`waiters`/`trigger`. |
| `Makefile` | Build, `clean`, `check`/`checkpatch`. |

## Build

```bash
cd 12_wait_queues_blocking
make
```

## Load and test

```bash
sudo insmod ./wait_queues_blocking.ko
cat /sys/module/wait_queues_blocking/parameters/interval_ms   # 3000 by default
```

Watch a blocking read actually block:

```bash
time cat /dev/blocking_demo
# real ~0-3s depending on where in the cycle you started, then:
# event #1
```

Trigger an event on demand instead of waiting for the timer:

```bash
cat /dev/blocking_demo &   # backgrounded, currently blocked
sleep 1
echo 1 | sudo tee /sys/class/misc/blocking_demo/trigger > /dev/null
wait   # the backgrounded cat returns immediately once triggered
```

Confirm the thundering-herd fix with two readers at once:

```bash
cat /dev/blocking_demo & cat /dev/blocking_demo &
sleep 1
echo 1 | sudo tee /sys/class/misc/blocking_demo/trigger > /dev/null
wait
# exactly ONE of the two prints an "event #N" line right away;
# the other keeps waiting for the *next* trigger/timer tick
```

Watch how many readers are currently blocked:

```bash
cat /dev/blocking_demo &   # blocks
sleep 0.5
cat /sys/class/misc/blocking_demo/waiters   # 1
wait
```

Non-blocking mode (`O_NONBLOCK`) — needs a tool that actually sets the
flag, unlike `cat`:

```bash
python3 - <<'EOF'
import os

fd = os.open("/dev/blocking_demo", os.O_RDONLY | os.O_NONBLOCK)
try:
    print(os.read(fd, 64))
except BlockingIOError as e:
    print("would block, as expected:", e)
os.close(fd)
EOF
```

`poll()`/`select()` across the wait, from Python (works the same way a C
`select(2)` call would):

```bash
python3 - <<'EOF'
import select

fd = open("/dev/blocking_demo", "rb", buffering=0)
print("waiting for readiness via select()...")
ready, _, _ = select.select([fd], [], [], 10)
print("ready:", bool(ready))
if ready:
    print(fd.read(64))
EOF
```

## checkpatch

```bash
make check
```

## Cleanup

```bash
sudo rmmod wait_queues_blocking
dmesg | tail -3
make clean
```

## Things to try

- Change `interval_ms` live (`echo 200 | sudo tee
  /sys/module/wait_queues_blocking/parameters/interval_ms`) and watch a
  backgrounded `while true; do cat /dev/blocking_demo; done` speed up
  immediately — the timer re-reads `interval_ms` fresh on every
  reschedule, the same "no store callback needed" behavior module
  [04](../04_module_params/) covers for a `0644` parameter.
- Start a blocking `cat /dev/blocking_demo` in one terminal, then hit
  Ctrl-C. It returns instantly, unlike a process stuck in uninterruptible
  (`D`) sleep — a direct, hands-on illustration of what
  `wait_event_interruptible()` buys you over the plain (non-interruptible)
  `wait_event()`.
- Rebuild with `wait_event_interruptible()` replaced by `wait_event()`
  (no signal handling) to feel the difference — with the timer running,
  behavior looks identical; the difference only shows up trying to
  interrupt a wait that's taking a while, which is exactly why it's easy
  to get this wrong in real drivers and only notice under specific
  conditions.

## Debugging with GDB

For a full, self-contained, step-by-step session for this module — tmux
pane layout, every command, every output explained — see
[`gdb_walkthrough.md`](gdb_walkthrough.md).

Setup: [`../gdb_debugging.md`](../gdb_debugging.md). Break on both sides
of the producer/consumer pair to see the context difference this module
is built around, directly rather than inferred from the `comm=`/`in_*=`
fields the driver itself logs:

```gdb
(gdb) lx-symbols /home/adiopocere/Desktop/codes/linux-kernel-project
(gdb) break producer_fn
(gdb) continue
```

When it hits (the timer firing): `print $lx_current()->comm` — note
it's whatever task happened to be running, *not* a thread belonging to
this driver. `bt` shows a softirq call chain
(`run_timer_softirq`/`__softirqentry_text_start`-ish frames) rather than
a syscall entry.

```gdb
(gdb) break bq_read
(gdb) continue
```
```bash
cat /dev/blocking_demo &
```

This time `print $lx_current()->comm` shows `cat`, and `bt` shows a real
syscall chain (`ksys_read` → `vfs_read` → `bq_read`). Step through the
`atomic_xchg(&data_ready, 0)` line with `next` and `print` its return
value before deciding whether the loop `break`s out or calls
`wait_event_interruptible()` — the exact check-and-clear moment the
thundering-herd fix in this module's README depends on.

**`bq_poll`/`trigger_store`/init/exit**, verified:

```bash
$ gdb -q -batch -nx -ex "file wait_queues_blocking.ko" \
    -ex "info line bq_poll" -ex "info line trigger_store" \
    -ex "info line wait_queues_blocking_init" -ex "info line wait_queues_blocking_exit" \
    wait_queues_blocking.ko
Line 114 ... <bq_poll> ...
Line 162 ... <trigger_store> ...
Line 187 ... <wait_queues_blocking_init> ...
Line 222 ... <wait_queues_blocking_exit> ...
```

`break bq_poll`, then drive a `select()`/`poll()` from a Python
one-liner against `/dev/blocking_demo` (this module's own README has one)
— `next` through `poll_wait()` and watch `atomic_read(&data_ready)`
decide the return value, the exact mechanism that makes `select()`
return "readable" at the right moment without the caller ever calling
`read()` first.

`break wait_queues_blocking_exit`, `rmmod`, and step through in order:
`timer_shutdown_sync(&producer_timer)` first, *then* the defensive
`wake_up_interruptible_all()` this module's own source comments explain is
mostly unreachable in practice (module refcounting via `fops.owner`
already prevents `rmmod` while a reader is blocked) — `print
$lx_current()->comm` here will always read `rmmod`, never a reader,
confirming that comment empirically.

