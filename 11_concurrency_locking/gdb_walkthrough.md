# GDB walkthrough — 11_concurrency_locking, hands-on, start to finish

`concurrency_locking.c` implements the same "increment a shared counter"
operation four ways — unsynchronized (`MODE_NONE`, deliberately racy),
`spinlock_t`, `struct mutex`, and `atomic64_t` — switchable at runtime
through sysfs. One honest limit up front: a KGDB break-in freezes
**every CPU** in the guest at once, so you cannot literally catch two
`write()` calls interleaved mid-flight the way a real race actually
happens. What GDB *can* do — and what this walkthrough is built around —
is stop cleanly inside the unsynchronized read-modify-write and show you
the exact instant the "vulnerable window" the race depends on is open,
then contrast it directly against the same code path under a real lock,
where that window structurally cannot exist.

Every command below says exactly which pane. One command per step,
always — paste it, wait for the prompt to come back, then the next one.

`increment_once()` — the function that actually contains all four
locking strategies — has no breakable symbol of its own; it's inlined
into `race_write()` (confirmed statically before this walkthrough was
written: `info line increment_once` reports an address *inside*
`race_write`, not a separate function). `break race_write` and
`next`-step through its body; there's no separate call to descend into.

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

## Step 1 — build it, copy onto the scratch disk

*Regular terminal (detach with `Ctrl-b d`, or a separate window).*

```bash
cd 11_concurrency_locking
make -C /home/adiopocere/Desktop/codes/linux_mainline M=$(pwd) modules
```
```bash
modinfo concurrency_locking.ko | grep vermagic
```
```bash
sudo mount -o loop /home/adiopocere/Desktop/codes/qemu-vmb/labs-disk.img /tmp/vmb-mnt
sudo mkdir -p /tmp/vmb-mnt/11_concurrency_locking
sudo cp concurrency_locking.ko /tmp/vmb-mnt/11_concurrency_locking/
sudo umount /tmp/vmb-mnt
```

(`stress_test`, the userspace pthread hammering tool, is only needed for
reproducing the race under real load *outside* GDB — see this module's
own `readme.md` for that. This walkthrough uses a single manual `echo`
per breakpoint hit instead, since a live GDB session and a
multi-threaded stress test fighting over the same lock at once would be
nearly impossible to reason about together.)

## Step 2 — boot the guest

**Pane: vmb**

```bash
qemu-system-aarch64 -M virt -cpu max -m 1024 -smp 2 \
  -kernel /home/adiopocere/Desktop/codes/linux_mainline/arch/arm64/boot/Image \
  -initrd /home/adiopocere/Desktop/codes/qemu-vmb/initramfs.cpio.gz \
  -drive file=/home/adiopocere/Desktop/codes/qemu-vmb/labs-disk.img,if=virtio,format=raw \
  -append "console=ttyAMA0 rdinit=/init nokaslr" -nographic -s
```

Wait for `=== VM B (QEMU) ready ===` and `~ #`.

## Step 3 — start gdb, connect

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

## Step 4 — load the module, confirm the default mode

**Pane: gdb**

```
break do_init_module
```
```
continue
```

**Pane: vmb**

```bash
insmod /mnt/labs/11_concurrency_locking/concurrency_locking.ko
```

**Pane: gdb**

```
lx-symbols /home/adiopocere/Desktop/codes/linux-kernel-project
```

**Pane: vmb**

```bash
cat /sys/class/misc/race_demo/mode
```

Should read `0` — `MODE_NONE`, racy by default.

## Step 5 — `MODE_NONE`: stand inside the vulnerable window

**Pane: gdb**

```
break race_write
```
```
continue
```

**Pane: vmb**

```bash
echo x | tee /dev/race_demo
```

**Pane: gdb**

```
Thread 2 hit Breakpoint 2, race_write (file=0x..., buf=0x..., count=2, ppos=0x...) at concurrency_locking.c:144
```
```
next
```
```
next
```
```
next
```
```
print tmp
```
```
$1 = 0
```
```
print counter_plain
```
```
$2 = 0
```

Right here — after `tmp` has been read but before `counter_plain` has
been written back — is the entire bug. `tmp` and `counter_plain` are
identical right now, but from this exact point until
`WRITE_ONCE(counter_plain, tmp + 1)` a few lines ahead, any *other*
writer that ran its own read-modify-write in between would have its
increment silently discarded the moment this one finally writes back.
The `cpu_relax()` loop you're about to step through exists purely to
widen this window in real (non-debugger) execution; under GDB,
single-threaded and stopped, you don't need the delay — you're standing
inside the window already.

```
next
```
```
next
```
```
print counter_plain
```
```
$3 = 1
```

## Step 6 — `MODE_SPINLOCK`: the same path, window closed

```
delete
```
```
y
```

**Pane: vmb**

```bash
echo 1 | tee /sys/class/misc/race_demo/mode
```

**Pane: gdb**

```
break race_write
```
```
continue
```

**Pane: vmb**

```bash
echo x | tee /dev/race_demo
```

**Pane: gdb**

```
Thread 2 hit Breakpoint 3, race_write (...) at concurrency_locking.c:144
```
```
next
```
```
next
```
```
next
```
```
print counter_spinlock
```
```
$4 = {rlock = {raw_lock = {locked = 1, ...}}}
```

`locked = 1` confirms the lock is genuinely held at this instant — not
"about to be," actually acquired, checkable directly rather than
inferred. Because a KGDB break-in stops every CPU including any that
would otherwise be spinning on this same lock, you can hold this
breakpoint indefinitely without anything timing out or deadlocking —
that's specific to the debugger freeze and would never happen in real
concurrent execution.

```
next
```
```
next
```
```
print counter_spinlock
```
```
$5 = {rlock = {raw_lock = {locked = 0, ...}}}
```

The entire read-modify-write happened between the `locked = 1` you saw
and the `locked = 0` you just saw, with no `next` boundary in between
where a second writer's `increment_once()` could interleave: any other
CPU attempting `spin_lock()` on this same lock during that window would
itself be forced to wait, not silently corrupt the value. This is the
literal, structural reason the race from step 5 cannot happen here.

## Step 7 — `MODE_MUTEX`: a sleeping lock instead of a spinning one

```
delete
```
```
y
```

**Pane: vmb**

```bash
echo 2 | tee /sys/class/misc/race_demo/mode
```

**Pane: gdb**

```
break race_write
```
```
continue
```

**Pane: vmb**

```bash
echo x | tee /dev/race_demo
```

**Pane: gdb**

```
next
```
```
next
```
```
print counter_mutex
```
```
$6 = {owner = {counter = ...}, ...}
```

`counter_mutex.owner` is non-zero while held — the actual `struct
task_struct *` of whoever holds it, packed with some low status bits
(`include/linux/mutex.h` has the exact encoding for this kernel
version). The practical consequence, per this module's own source
comment on `MODE_MUTEX`: safe only from process context, never from an
interrupt handler, because a `mutex_lock()` that has to wait genuinely
reschedules this task off the CPU rather than busy-spinning. `write()`
always runs in ordinary process context, which is why this driver may
use a mutex here at all — module 14's timer callback runs from softirq
context and could not legally take this same kind of lock.

## Step 8 — `MODE_ATOMIC`: no lock, still no race

```
delete
```
```
y
```

**Pane: vmb**

```bash
echo 3 | tee /sys/class/misc/race_demo/mode
```

**Pane: gdb**

```
break race_write
```
```
continue
```

**Pane: vmb**

```bash
echo x | tee /dev/race_demo
```

**Pane: gdb**

```
next
```
```
next
```
```
print counter_atomic
```
```
$7 = {counter = 1}
```

There's no intermediate step to stop between here — `next` runs the
whole increment in one line, because on real hardware it compiles down
to a single atomic instruction (an `LDADD`-class instruction on
`arm64`). No lock, but also no window: the "entire operation" really is
one indivisible step. Contrast this directly with `MODE_NONE`'s read
*then* delay *then* write from step 5 — atomic addition is a hardware
primitive for exactly this one operation, which is also precisely why
it stops being sufficient the moment you need to combine more than one
related value atomically.

## Step 9 — `mode_store`: the sysfs write path

```
delete
```
```
y
```
```
break mode_store
```
```
continue
```

**Pane: vmb**

```bash
echo 0 | tee /sys/class/misc/race_demo/mode
```

**Pane: gdb**

```
Thread 2 hit Breakpoint N, mode_store (...) at concurrency_locking.c:193
```
```
next
```
```
print value
```
```
next
```
```
next
```

`reset_store` (a separate function, line 225) is worth knowing about
even without stepping through it: it takes `counter_spinlock` to zero
`counter_plain`, but calls `atomic64_set()` on `counter_atomic` with
**no lock at all** — a single atomic write, like the atomic increment
in step 8, needs no external synchronization. Two different reset
mechanisms for two different concurrency strategies, both defensible for
the same reason.

## Step 10 — the exit path

`concurrency_locking_exit` is marked `__exit`, placed in its own
`.exit.text` section, which `lx-symbols` never relocates — `break
concurrency_locking_exit` right now would silently resolve to a raw,
unrelocated file offset. Break on the generic unload hook instead:

```
delete
```
```
y
```
```
break __do_sys_delete_module
```
```
continue
```

**Pane: vmb**

```bash
rmmod concurrency_locking
```

**Pane: gdb**

```
advance kernel/module/main.c:863
```
```
print mod->exit
```
```
$8 = (void (*)(void)) 0xffff80007c3205b8
```

(Your address will differ — module memory placement is random per boot
even with `nokaslr`. Use whatever `print mod->exit` gives you.)

```
add-symbol-file /home/adiopocere/Desktop/codes/linux-kernel-project/11_concurrency_locking/concurrency_locking.ko -s .exit.text 0xffff80007c3205b8
```
```
y
```
```
break concurrency_locking_exit
```
```
Breakpoint N at 0xffff80007c3205b8: file concurrency_locking.c, line 269. (2 locations)
```
```
disable N.1
```
```
continue
```

**Pane: vmb**

```bash
rmmod concurrency_locking
```

**Pane: gdb**

```
Thread N hit Breakpoint N.2, concurrency_locking_exit () at concurrency_locking.c:269
269		sysfs_remove_group(&race_miscdev.this_device->kobj, &race_attr_group);
```

## Step 11 — clean up

```
delete
```
```
y
```

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

The four modes aren't four different algorithms producing the same
answer by coincidence — they're four different answers to "what can
happen between the read and the write," and GDB lets you stand *inside*
that gap and inspect the lock's own state (`locked`, `owner`, or the
absence of any lock object at all for atomics) at the precise instant
the gap is open or closed (steps 5–8). `MODE_NONE`'s bug isn't a subtle
timing coincidence once you've actually stood inside its window with a
debugger — it's a structural hole the other three modes each close by a
different, verifiable mechanism.
