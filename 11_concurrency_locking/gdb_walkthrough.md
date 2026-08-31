# GDB walkthrough — 11_concurrency_locking

`concurrency_locking.c` implements the same "increment a shared
counter" operation four ways — unsynchronized (`MODE_NONE`,
deliberately racy), `spinlock_t`, `struct mutex`, and `atomic64_t` —
switchable at runtime through sysfs. One honest limit up front: a
KGDB break-in freezes **every CPU** in the guest at once (see
[`../gdb_debugging.md`](../gdb_debugging.md)'s gotchas section), so you
cannot literally catch two `write()` calls interleaved mid-flight the
way a real race actually happens. What GDB *can* do — and what this
walkthrough is actually built around — is stop cleanly inside the
unsynchronized read-modify-write and show you the exact instant the
"vulnerable window" the race depends on is open, then contrast it
directly against the same code path under a real lock, where that
window structurally cannot exist.

## Environment

```bash
cd 11_concurrency_locking
make -C /home/adiopocere/Desktop/codes/linux_mainline M=$(pwd) modules
modinfo concurrency_locking.ko | grep vermagic
sudo mount -o loop /home/adiopocere/Desktop/codes/qemu-vmb/labs-disk.img /tmp/vmb-mnt
sudo mkdir -p /tmp/vmb-mnt/11_concurrency_locking
sudo cp concurrency_locking.ko /tmp/vmb-mnt/11_concurrency_locking/
sudo umount /tmp/vmb-mnt
```

`stress_test` (the userspace pthread hammering tool) is only needed
for reproducing the race under real load *outside* GDB — see the
lab's own [readme.md](readme.md) for that. This walkthrough uses a
single manual `echo` per breakpoint hit instead, since a live GDB
session and a multi-threaded stress test fighting over the same lock
at the same time would be nearly impossible to reason about together.

## tmux layout

Standard `vmb` + `gdbsess` — [`../gdb_debugging.md`](../gdb_debugging.md).

## Real, verified breakpoint targets

```
Line 144: race_write               (increment_once() is inlined into this - see below)
Line 193: mode_store
Line 225: reset_store
Line 247: concurrency_locking_init
Line 269: concurrency_locking_exit
```

`increment_once()` — the function that actually contains all four
locking strategies — has no breakable symbol of its own:

```
$ gdb -q -batch -nx -ex "file concurrency_locking.ko" -ex "info line increment_once" concurrency_locking.ko
Line ... of "concurrency_locking.c" starts at address 0x... <race_write+NN> ...
```

It resolves *inside* `race_write`, confirming GCC inlined it entirely
— the same pattern as lab 13's `do_allocate()` and lab 07's
`printk_emit_all_levels()`. `break race_write` and `next`-step through
its body; there is no separate call to descend into.

## The walkthrough

### Step 1 — load and confirm the default mode

```gdb
(gdb) target remote :1234
(gdb) lx-version
(gdb) break do_init_module
(gdb) continue
```
```bash
# vmb:
insmod /mnt/labs/11_concurrency_locking/concurrency_locking.ko
```
```gdb
(gdb) lx-symbols /home/adiopocere/Desktop/codes/linux-kernel-project
```
```bash
# vmb:
cat /sys/class/misc/race_demo/mode
```
Should read `0` — `MODE_NONE`, racy by default.

### Step 2 — the vulnerable window, `MODE_NONE`

```gdb
(gdb) break race_write
(gdb) continue
```
```bash
# vmb:
echo x | tee /dev/race_demo
```
```gdb
Thread 2 hit Breakpoint N, race_write (file=0x..., buf=0x..., count=2, ppos=0x...) at concurrency_locking.c:144
(gdb) next               # `size_t n = min_t(...)` and into the for loop
(gdb) next                # into increment_once()'s inlined body - the READ_ONCE(mode) switch
(gdb) next                 # `tmp = READ_ONCE(counter_plain);`
(gdb) print tmp
$1 = 0
(gdb) print counter_plain
$2 = 0
```

Right here — after `tmp` has been read but before `counter_plain` has
been written back — is the entire bug. `tmp` and `counter_plain` are
identical right now, but from this exact point until the
`WRITE_ONCE(counter_plain, tmp + 1)` a few lines ahead, any *other*
writer that managed to run its own read-modify-write in between would
have its increment silently discarded the moment this one finally
writes back. The `cpu_relax()` loop you're about to step through exists
purely to widen this window in real (non-debugger) execution, so the
race reproduces reliably under `stress_test` instead of needing
hundreds of runs to hit by luck — under GDB, single-threaded and
stopped, you don't need the delay to see the window; you're standing
inside it already.

```gdb
(gdb) next    # the cpu_relax() loop - `next` runs it to completion, doesn't step each iteration
(gdb) next     # WRITE_ONCE(counter_plain, tmp + 1)
(gdb) print counter_plain
$3 = 1
```

### Step 3 — the same code path, `MODE_SPINLOCK`, and why the window is gone

```gdb
(gdb) delete
```
```bash
# vmb:
echo 1 | tee /sys/class/misc/race_demo/mode
```
```gdb
(gdb) break race_write
(gdb) continue
```
```bash
# vmb:
echo x | tee /dev/race_demo
```
```gdb
Thread 2 hit Breakpoint N, race_write (...) at concurrency_locking.c:144
(gdb) next
(gdb) next    # into increment_once, READ_ONCE(mode) switch now takes the MODE_SPINLOCK case
(gdb) next     # spin_lock(&counter_spinlock)
(gdb) print counter_spinlock
$4 = {rlock = {raw_lock = {locked = 1, ...}}}   # exact fields vary by lock implementation/arch
```

`locked = 1` (or your kernel's equivalent representation) confirms the
lock is genuinely held at this instant — not just "about to be," but
actually acquired, checkable directly rather than inferred. Because a
KGDB break-in stops every CPU including any that would otherwise be
spinning on this same lock, you can hold this breakpoint indefinitely
without anything timing out or deadlocking; that's specific to the
debugger freeze and would never happen in real concurrent execution.

```gdb
(gdb) next    # counter_plain++
(gdb) next     # spin_unlock
(gdb) print counter_spinlock
$5 = {rlock = {raw_lock = {locked = 0, ...}}}
```

The entire read-modify-write — read, increment, write — happened
between the `locked = 1` you saw and the `locked = 0` you just saw,
with no `next` boundary in between where a second writer's
`increment_once()` could interleave and still be legal: any other CPU
attempting `spin_lock()` on this same lock during that window would
itself be forced to wait, not silently corrupt the value. This is the
literal, structural reason the race from step 2 cannot happen here —
not "less likely," genuinely cannot, because there is no unlocked
state between the read and the write for another writer to land in.

### Step 4 — `MODE_MUTEX`, and the one real difference from spinlock

```gdb
(gdb) delete
```
```bash
# vmb:
echo 2 | tee /sys/class/misc/race_demo/mode
```
```gdb
(gdb) break race_write
(gdb) continue
```
```bash
# vmb:
echo x | tee /dev/race_demo
```
```gdb
(gdb) next
(gdb) next    # mutex_lock(&counter_mutex)
(gdb) print counter_mutex
$6 = {owner = {counter = ...}, ...}    # owner field encodes the owning task_struct pointer
```

`counter_mutex.owner` is non-zero while held — the actual `struct
task_struct *` of whoever holds it, packed with some low status bits
(check `include/linux/mutex.h`'s comment on `struct mutex` in
`../../linux_mainline` for the exact encoding on this kernel version).
The source's own comment on `MODE_MUTEX` states the practical
consequence of this being a sleeping lock rather than a spinning one:
safe only from process context — never from an interrupt handler —
because a `mutex_lock()` that has to wait genuinely reschedules this
task off the CPU rather than busy-spinning. `write()` always runs in
ordinary process context, which is why this driver is allowed to use
it here at all; lab 14's timer callback runs from softirq context and
could not legally take this same kind of lock.

### Step 5 — `MODE_ATOMIC`, and why "no lock" still isn't a race here

```gdb
(gdb) delete
```
```bash
# vmb:
echo 3 | tee /sys/class/misc/race_demo/mode
```
```gdb
(gdb) break race_write
(gdb) continue
```
```bash
# vmb:
echo x | tee /dev/race_demo
```
```gdb
(gdb) next
(gdb) next    # atomic64_inc(&counter_atomic) - a single instruction, no read/modify/write to straddle
(gdb) print counter_atomic
$7 = {counter = 1}
```

There's no intermediate step to stop between here — `next` runs the
whole increment in one line, because on real hardware it compiles down
to a single atomic instruction (an `LDADD`-class instruction on
`arm64`, this guest's architecture — `disassemble` the surrounding
lines if you want to see it directly). No lock, but also no window:
the "entire operation" really is one indivisible step. Contrast this
directly with `MODE_NONE`'s read *then* delay *then* write from step
2 — the fundamental difference is that atomic addition is a hardware
primitive for exactly this one operation, which is also precisely why
it stops being sufficient the moment you need to combine more than one
related value atomically (a general limitation, not specific to this
driver).

### Step 6 — `mode_store`/`reset_store`, briefly

```gdb
(gdb) delete
(gdb) break mode_store
(gdb) continue
```
```bash
# vmb:
echo 0 | tee /sys/class/misc/race_demo/mode
```
```gdb
Thread 2 hit Breakpoint N, mode_store (...) at concurrency_locking.c:193
(gdb) next   # kstrtoint(buf, 0, &value)
(gdb) print value
(gdb) next    # the MODE_NONE..MODE_ATOMIC range check
(gdb) next     # WRITE_ONCE(mode, value) - the actual switch, visible to every subsequent race_write
```

`reset_store` (line 225) is worth noting for one specific detail:
it takes `counter_spinlock` to zero `counter_plain`, but calls
`atomic64_set()` on `counter_atomic` with **no lock at all** — because
a single atomic write, like the atomic increment in step 5, needs no
external synchronization. Two different reset mechanisms for two
different concurrency strategies, both defensible for the same reason.

## Cleanup

```gdb
(gdb) delete
```
```bash
# vmb:
rmmod concurrency_locking
poweroff -f
```

## What this proves

The four modes aren't four different algorithms producing the same
answer by coincidence — they're four different answers to "what can
happen between the read and the write," and GDB lets you stand
*inside* that gap and inspect the lock's own state (`locked`, `owner`,
or the absence of any lock object at all for atomics) at the precise
instant the gap is open or closed. `MODE_NONE`'s bug isn't a subtle
timing coincidence once you've actually stood inside its window with a
debugger — it's a structural hole the other three modes each close by
a different, verifiable mechanism.
