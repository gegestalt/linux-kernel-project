# 11 — concurrency_locking

A shared counter, deliberately racy by default, fixed three different ways
— `spinlock_t`, `struct mutex`, `atomic_t` — switchable at runtime, plus a
multi-threaded stress test that turns "the race is a Heisenbug" into "the
race fails the test almost every single run."

## What this demonstrates

- **A genuine lost-update race.** In `MODE_NONE`, `increment_once()` reads
  the shared counter into a local variable, burns some cycles
  (`cpu_relax()` in a loop — see the comment in the code for why), then
  writes `local + 1` back. Two threads doing this concurrently can both
  read the same starting value and both write back the same `+1` result,
  silently losing one of the two increments. This is *the* canonical
  concurrency bug, and this lab makes it happen on purpose, reliably,
  instead of waiting for it to happen to you by accident in a driver
  you're trying to ship.
- **`spinlock_t`** (`spin_lock()`/`spin_unlock()`) — busy-waits rather than
  sleeping. Cheap for short critical sections; only safe where the holder
  never sleeps while holding it (and, in the general case, never valid to
  take from a context where sleeping is already forbidden without the
  `_irqsave` variant — this lab's critical section is short enough not to
  need that here, but see [14](../14_timers_workqueues/) for contexts
  where it would).
- **`struct mutex`** (`mutex_lock()`/`mutex_unlock()`) — may sleep if
  contended, parking the waiter on a wait queue instead of spinning. Only
  valid from process context, which `write()` always is.
- **`atomic_t`/`atomic64_t`** (`atomic64_inc()`) — no explicit lock at all;
  the increment is a single hardware-guaranteed atomic instruction. The
  fastest of the three correct options here, but it only works because
  "add one" is the entire operation — anything that needs to combine more
  than one related value atomically generally still needs a real lock.
- **Runtime-switchable locking strategy** via a sysfs attribute
  (`mode`), so one stress test binary can be pointed at all four
  implementations without touching the module.
- A stress test that measures, doesn't just assert: it computes the
  *expected* total (`threads × increments_per_thread`) and compares it
  against what the kernel actually recorded, reporting exactly how many
  updates were lost — not just pass/fail.

## Files

| File | Purpose |
|---|---|
| `concurrency_locking.c` | The driver: `/dev/race_demo` (write = N increments, read = current value) and sysfs `mode`/`counter`/`reset`. |
| `stress_test.c` | Userspace: spawns `pthread`s that hammer `write()` concurrently, then checks the final count against the expected total. |
| `Makefile` | Builds the module and `stress_test` (linked with `-pthread`); `check`/`checkpatch` covers both. |

## Build

```bash
cd 11_concurrency_locking
make
nproc     # worth knowing — the race is easiest to observe with 2+ CPUs available to the guest
```

## Load and test

```bash
sudo insmod ./concurrency_locking.ko
cat /sys/class/misc/race_demo/mode      # 0 = none (the default: racy on purpose)
```

Reproduce the race:

```bash
sudo ./stress_test 8 20000
# mode (from sysfs) = 0  (0=none 1=spinlock 2=mutex 3=atomic)
# threads=8 increments_per_thread=20000
# expected=160000 actual=<something smaller> lost=<something >0>
# RACE DETECTED: <N> update(s) were lost
```

If it doesn't lose anything on the first try, increase the load (more
threads, more increments) and try again — the race's odds depend on how
much your CPU count and scheduler actually interleave the threads:

```bash
sudo ./stress_test 32 50000
```

Now fix it, one mode at a time, always resetting first so a stale value
from the previous mode's counter storage doesn't confuse the result:

```bash
echo 1 | sudo tee /sys/class/misc/race_demo/mode > /dev/null   # spinlock
sudo ./stress_test 32 50000
# expected=1600000 actual=1600000 lost=0
# no lost updates

echo 2 | sudo tee /sys/class/misc/race_demo/mode > /dev/null   # mutex
sudo ./stress_test 32 50000

echo 3 | sudo tee /sys/class/misc/race_demo/mode > /dev/null   # atomic
sudo ./stress_test 32 50000
```

Read straight from `/dev/race_demo` as well as sysfs, to see they agree:

```bash
cat /dev/race_demo
cat /sys/class/misc/race_demo/counter
```

## checkpatch + userspace lint

```bash
make check
```

## Cleanup

```bash
sudo rmmod concurrency_locking
dmesg | tail -3    # final counter value and how many times it was reset
make clean            # also removes the stress_test binary
```

## Things to try

- Loop mode 0 with a small, fixed load (say `8 5000`) ten times in a row
  and record whether it loses updates every time, most times, or only
  occasionally — this is the shape of exactly the kind of intermittent bug
  that makes concurrency issues so unpleasant to debug without a
  reproducer this deliberate.
- Delete the `cpu_relax()` loop from `MODE_NONE` and rerun the same stress
  test. On a fast, lightly-loaded machine the race may become much harder
  to trigger even though the code is exactly as broken — a direct
  demonstration of why "it passed my testing" is not evidence of
  correctness for concurrent code.
- Time all four modes against each other with something like `time sudo
  ./stress_test 32 200000` per mode. Expect roughly: none (fastest, and
  wrong) < atomic < spinlock < mutex, though the exact ordering depends on
  contention level and core count — the point isn't the exact numbers,
  it's building the intuition for what each primitive actually costs.
- Read `Documentation/locking/locktypes.rst` in `../../linux_mainline` for
  the real rules on which lock types are legal in which contexts
  (interrupt handlers, softirqs, `PREEMPT_RT` kernels change some of
  this) — this lab only exercises the simplest, always-safe case:
  process-context code taking its own lock around its own data.
