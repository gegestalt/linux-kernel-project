# GDB walkthrough — 12_wait_queues_blocking, hands-on, start to finish

`wait_queues_blocking.c` pairs a kernel timer (softirq context, producing
an "event" every `interval_ms`) with a blocking `read()`
(`wait_event_interruptible()`) and a `poll()` callback. This is the first
module whose central object is a **task that isn't running** — a reader
blocked in `bq_read()` has voluntarily taken itself off the CPU
entirely, parked on `event_wq` until something wakes it. KGDB's break-in
freezes every CPU regardless of what's running on it, which means it can
catch a *sleeping* task exactly as easily as a running one — this
walkthrough is built around seeing what that actually looks like.

Every command below says exactly which pane. One command per step,
always — paste it, wait for the prompt to come back, then the next one.
You'll want a **third** way to interact with the guest for step 8 — a
second shell inside the guest if your busybox setup supports it, or just
backgrounding commands with `&` in the same shell.

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
cd 12_wait_queues_blocking
make -C /home/adiopocere/Desktop/codes/linux_mainline M=$(pwd) modules
```
```bash
modinfo wait_queues_blocking.ko | grep vermagic
```
```bash
sudo mount -o loop /home/adiopocere/Desktop/codes/qemu-vmb/labs-disk.img /tmp/vmb-mnt
sudo mkdir -p /tmp/vmb-mnt/12_wait_queues_blocking
sudo cp wait_queues_blocking.ko /tmp/vmb-mnt/12_wait_queues_blocking/
sudo umount /tmp/vmb-mnt
```

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

## Step 4 — load the module with a short timer interval

**Pane: gdb**

```
break do_init_module
```
```
continue
```

**Pane: vmb**

```bash
insmod /mnt/labs/12_wait_queues_blocking/wait_queues_blocking.ko interval_ms=1500
```

**Pane: gdb**

```
lx-symbols /home/adiopocere/Desktop/codes/linux-kernel-project
```

## Step 5 — the producer: a timer callback caught mid-softirq

**Pane: gdb**

```
break producer_fn
```
```
continue
```

Don't touch `vmb` — `producer_fn` fires on its own, every `interval_ms`:

```
Thread 2 hit Breakpoint 2, producer_fn (t=0x...) at wait_queues_blocking.c:50
```
```
bt
```

This backtrace is the concrete evidence behind the source's own comment
("timer callbacks run in softirq context: no sleeping, no blocking
allocations") — every frame below `producer_fn` is timer/softirq
machinery, not a task's own thread. Live-tested on this repo's own
kernel, the real chain went considerably deeper than a textbook
`call_timer_fn` → `run_timer_softirq` sketch:

```
#0  producer_fn (...) at wait_queues_blocking.c:50
#1  call_timer_fn (...) at kernel/time/timer.c:1748
#2  expire_timers (...) at kernel/time/timer.c:1799
...
#6  timer_expire_remote (cpu=1) at kernel/time/timer.c:2136
#7  tmigr_handle_remote_cpu (...) at kernel/time/timer_migration.c:985
...
#12 run_timer_softirq () at kernel/time/timer.c:2409
#13 handle_softirqs (...) at kernel/softirq.c:645
#14 __do_softirq () at kernel/softirq.c:679
...
Backtrace stopped: previous frame inner to this frame (corrupt stack?)
```

The `timer_expire_remote`/`tmigr_*` frames are the NOHZ timer-migration
subsystem: this timer's *nominal* CPU was idle, so a different CPU's
softirq is expiring it "remotely" rather than waking the idle CPU just
to run one timer. Two things here are artifacts, not bugs: frames tagged
`[PAC]` are arm64 pointer authentication on the return address (GDB
strips it to unwind, tagging the frame so you know it did), and the
`Backtrace stopped: ... (corrupt stack?)` line at the bottom is GDB
hitting the irq-stack-switch boundary in `call_on_irq_stack`, not a real
stack corruption.

```
print $lx_current().comm
```
```
print $lx_current().pid
```

Plain `current` is not visible to GDB as an ordinary C symbol over a
remote kernel target (it's backed by an arch-specific per-CPU access,
not a global variable) — `print current->comm` would fail with `No
symbol "current" in current context`; `$lx_current()` is `lx-symbols`'s
convenience function that reads it correctly. Live-tested, this printed
`"swapper/0"` / `0` — the idle task, not `insmod` or anything meaningfully
related to this timer, confirming the source comment directly.

```
next
```
```
next
```
```
print event_id
```

(Exact `next` counts here aren't reliable — `atomic64_inc()` on arm64 is
commonly emitted through an alternative-patched LSE atomic instruction
sequence, and a `next` can briefly land you inside
`arch/arm64/include/asm/alternative-macros.h` before returning to this
file. Watch the line GDB actually shows you rather than counting steps.)

```
next
```
```
next
```
```
next
```

(`atomic_set(&data_ready, 1)`, then `wake_up_interruptible(&event_wq)` —
about to matter in step 6 — then `mod_timer()` reschedules itself for
the next interval.)

## Step 6 — catch a task actually blocked on the wait queue

This is the step this module exists for.

```
delete
```
```
y
```
```
break bq_read
```
```
continue
```

**Pane: vmb**

```bash
cat /dev/blocking_demo &
```

**Pane: gdb**

```
Thread 2 hit Breakpoint 3, bq_read (...) at wait_queues_blocking.c:76
```

**Check for a pending event before going further — this is the real
trap in this step.** `producer_fn` has been firing every 1500ms the
whole time you were single-stepping through step 5, whether you were
watching it or not:

```
print data_ready
```

If that reads `{counter = 1}` (expect it to, most of the time),
`atomic_xchg(&data_ready, 0)` a few lines down takes the *non-blocking*
fast path and returns immediately, never touching
`wait_event_interruptible()` at all. Confirm it, on purpose, as a real
demonstration of that other branch:

```
finish
```

This returns the already-available event straight away. `data_ready` is
now `0` again. Start a fresh read immediately, before the next tick sets
it again:

**Pane: vmb**

```bash
cat /dev/blocking_demo &
```

**Pane: gdb**

```
continue
```

You should land back in `bq_read` with `data_ready` genuinely `0` this
time (`print data_ready` again if you like). Now step through for real:

```
next
```
```
next
```
```
print waiter_count
```
```
$1 = {counter = 0}
```

(GDB's `next` stops *before* executing the line it displays, not after —
`waiter_count` is still `0` here, the pre-increment value.)

```
next
```

**This `next` is the one to watch closely.**
`wait_event_interruptible()` is a macro that, if the condition is false,
actually calls `schedule()` — this task genuinely leaves the CPU here.
GDB may show it "completing" only once the condition becomes true,
because a real reschedule happened underneath it — if it seems to hang,
that's not a bug, it's this task legitimately asleep. `continue` instead
of `next` lets guest wall-clock time pass normally while it waits
(remember: the guest is otherwise frozen the whole time GDB is stopped
anywhere).

```
lx-ps
```

Find `cat` in the listing — its state should read `INTERRUPTIBLE`
(kernel `TASK_INTERRUPTIBLE`), not `RUNNING`. Compare this against
`producer_fn`'s task in step 5, or against `insmod` in step 4 — those
were always `RUNNING` (or briefly preempted) at the moment you caught
them, because a syscall's own callback code was still actively
executing. This `cat` is different: it has explicitly told the scheduler
"wake me only when this condition is true," and handed control away in
the meantime.

## Step 7 — wake it up, watch the *pending* `next` resume on its own

With the reader still parked from step 6, trigger an event from the
sysfs trigger rather than waiting out the timer. Step 6's `cat` was
started with `&`, so the vmb shell prompt is free — no second session
needed, same pane:

**Pane: vmb**

```bash
echo 1 | tee /sys/class/misc/blocking_demo/trigger
```

**Don't type anything new into the gdb pane for this step — just look at
it.** The `next` you issued at the end of step 6 (on the
`wait_event_interruptible(...)` line) is still pending — that's what's
been sitting inside `schedule()` this whole time. As soon as
`wake_up_interruptible(&event_wq)` makes the wait condition true, that
outstanding `next` completes on its own:

```
77          char kbuf[64];
```

(or wherever the DWARF line table lands — the key point is you're
*back*, in the same call, with the same local variables, having crossed
a real sleep/wake/reschedule boundary without GDB losing track of
anything.)

```
print waiter_count
```
```
$2 = {counter = 0}
```

`atomic_dec_return` already ran on the way out of the wait.

```
next
```
```
print id
```

## Step 8 — `poll()`: readiness without blocking

```
delete
```
```
y
```
```
break bq_poll
```
```
continue
```

Any tool that uses `select()`/`poll()`/`epoll()` against
`/dev/blocking_demo` triggers this — a plain `cat` never will, since
`cat` just calls `read()` directly. If your busybox build lacks a
convenient poll-driving tool, run a Python one-liner from `vmb` instead
(see this module's own `readme.md`), or read the mechanics statically:

```
next
```
```
next
```
```
finish
```

`poll_wait()` itself never sleeps — it just tells the *caller*
(`select()`/`poll()`'s own generic kernel code) which wait queue to
watch. The actual blocking, if any, happens one layer up, potentially
across many file descriptors from many different drivers at once, which
is precisely why this callback has to be non-blocking itself.

## Step 9 — the exit path

`wait_queues_blocking_exit` is marked `__exit`, placed in its own
`.exit.text` section, which `lx-symbols` never relocates — `break
wait_queues_blocking_exit` right now would silently resolve to a raw,
unrelocated file offset, no error, it just never fires. Break on the
generic unload hook instead:

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

**Pane: vmb** (make sure no backgrounded `cat` still holds the device
open — `kill %1` or similar first; `fops.owner = THIS_MODULE` means
every open fd holds a module reference, and `rmmod` refuses to unload a
module with a nonzero reference count)

```bash
rmmod wait_queues_blocking
```

**Pane: gdb**

```
advance kernel/module/main.c:863
```
```
print mod->exit
```
```
$3 = (void (*)(void)) 0xffff80007c320640
```

(Your address will differ — module memory placement is random per boot
even with `nokaslr`.)

```
add-symbol-file /home/adiopocere/Desktop/codes/linux-kernel-project/12_wait_queues_blocking/wait_queues_blocking.ko -s .exit.text 0xffff80007c320640
```
```
y
```
```
break wait_queues_blocking_exit
```
```
Breakpoint N at 0x170: wait_queues_blocking_exit. (2 locations)
```
```
disable N.1
```
```
continue
```

**Pane: vmb**

```bash
rmmod wait_queues_blocking
```

**Pane: gdb**

```
Thread N hit Breakpoint N.2, 0xffff80007c320644 in cleanup_module ()
```

Reports as `cleanup_module`, not `wait_queues_blocking_exit` —
`module_exit()` aliases the function to the legacy name too, the same
mechanism modules 01/02 cover for `init_module`. `next` inside
`cleanup_module` won't have line-by-line resolution (the section
relocation fixes symbol/breakpoint addresses, not the DWARF line table
for code inside it) — GDB will say it's "single stepping until exit from
function cleanup_module, which has no line number information." That's
fine: `next` still correctly runs until it lands in the next fully-
resolved `vmlinux` function it calls:

```
next
```
```
timer_shutdown_sync (timer=0x... <producer_timer>) at kernel/time/timer.c:1717
```
```
finish
```
```
Value returned is $4 = 1
```

`1` means a pending/active timer actually existed and was cancelled by
this call (`0` would mean it had already fired, nothing to cancel) —
directly confirms the ordering the source comment warns about: this
runs *before* the module's data is torn down, specifically so a
still-in-flight `producer_fn()` can't race the unload.

## Step 10 — clean up

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

A blocked `read()` is not "still running, just slow" — `lx-ps` showing
`TASK_INTERRUPTIBLE` instead of `RUNNING` is direct evidence the
scheduler has genuinely removed this task from the CPU (step 6), and
`continue`/a pending `next` from a breakpoint set *before* the block can
carry you across a real sleep/wake cycle spanning wall-clock time and a
completely separate trigger, landing back in the same function with the
same local variables it had before, resumed rather than restarted
(step 7). Contrast this against every synchronous callback in modules
01–11: those never left the CPU at all between entry and return.
