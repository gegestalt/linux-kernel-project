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

## Debugging with GDB

For a full, self-contained, step-by-step session for this lab — tmux
pane layout, every command, every output explained — see
[`gdb_walkthrough.md`](gdb_walkthrough.md).

Setup: [`../gdb_debugging.md`](../gdb_debugging.md). One honest caveat
first: a KGDB break-in stops **every CPU**, so you cannot literally
watch two `write()` calls interleave live — the value of GDB here is
inspecting the *vulnerable window* and the lock state directly, not
catching a race in the act.

```gdb
(gdb) lx-symbols /home/adiopocere/Desktop/codes/linux-kernel-project
(gdb) break race_write
(gdb) continue
```
```bash
echo 0 | sudo tee /sys/class/misc/race_demo/mode   # MODE_NONE
echo x | sudo tee /dev/race_demo
```
```gdb
(gdb) next               # step in from race_write - increment_once() is inlined, so `next`
(gdb) next                # walks straight through its body, no separate breakpoint needed
(gdb) next               # ... until just past `tmp = READ_ONCE(counter_plain);`
(gdb) print tmp            # the value this "thread" has locally cached
(gdb) print counter_plain   # the shared value - identical right now, but about to diverge
```

(`break increment_once` directly would fail: `increment_once()` is
`static` and small enough that GCC inlines it entirely into
`race_write()`, leaving no symbol of its own — confirmed via
`gdb -batch -ex "info line increment_once"` reporting an address
*inside* `race_write`, not a separate function.)

Right here is the entire bug: any other writer that ran between this
`next` and the `WRITE_ONCE(counter_plain, tmp + 1);` a few lines down
would have its increment silently overwritten. Switch `mode` to `1`
(spinlock) or `2` (mutex), re-break, and compare: `print counter_spinlock`
/ `print counter_mutex` show the lock actually held (non-zero/owned)
for the whole read-modify-write, which is the structural reason the race
can't happen in those modes — there's no window to be caught in.

**`mode_store`/`reset_store`/init/exit**, all verified:

```bash
$ gdb -q -batch -nx -ex "file concurrency_locking.ko" \
    -ex "info line mode_store" -ex "info line reset_store" \
    -ex "info line concurrency_locking_init" -ex "info line concurrency_locking_exit" \
    concurrency_locking.ko
Line 193 ... <mode_store> ...
Line 225 ... <reset_store> ...
Line 247 ... <concurrency_locking_init> ...
Line 269 ... <concurrency_locking_exit> ...
```

```gdb
(gdb) break mode_store
(gdb) continue
```
```bash
echo 3 | sudo tee /sys/class/misc/race_demo/mode
```
```gdb
(gdb) print value            # kstrtoint()'d from "3" - not the raw string
(gdb) next                    # the `value < MODE_NONE || value > MODE_ATOMIC` range check
(gdb) next                     # WRITE_ONCE(mode, value) - the actual switch
(gdb) finish
```

`break reset_store`, trigger with `echo 1 | sudo tee
/sys/class/misc/race_demo/reset`, and step through `reset_counters()` —
note it takes `counter_spinlock` to zero `counter_plain` *and* separately
calls `atomic64_set()` on `counter_atomic` with no lock at all, because
an atomic write needs none. Two different reset mechanisms for two
different concurrency strategies, both real code you can watch run.

