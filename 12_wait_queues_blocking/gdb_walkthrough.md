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

Standard `vmb` + `gdbsess` — [`../gdb_debugging.md`](../gdb_debugging.md).
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
```

This backtrace is the concrete evidence behind the source comment
("Timer callbacks run in softirq context: no sleeping, no blocking
allocations"): every frame below `producer_fn` is timer/softirq
machinery, not a task's own thread. On a real kernel, expect this to be
considerably deeper than a textbook `call_timer_fn` →
`run_timer_softirq` → `__do_softirq` sketch — this repo's own kernel,
live-tested, actually showed:

```
#0  producer_fn (...) at wait_queues_blocking.c:50
#1  call_timer_fn (...) at kernel/time/timer.c:1748
#2  expire_timers (...) at kernel/time/timer.c:1799
#3  __run_timers (...) at kernel/time/timer.c:2374
#4  __run_timer_base (...) at kernel/time/timer.c:2386
#5  __run_timer_base (...) at kernel/time/timer.c:2378
#6  timer_expire_remote (cpu=1) at kernel/time/timer.c:2136
#7  tmigr_handle_remote_cpu (...) at kernel/time/timer_migration.c:985
#8  tmigr_handle_remote_up (...) at kernel/time/timer_migration.c:1076
#9  __walk_groups_from (...) at kernel/time/timer_migration.c:564
#10 __walk_groups (...) at kernel/time/timer_migration.c:581
#11 tmigr_handle_remote () at kernel/time/timer_migration.c:1135
#12 run_timer_softirq () at kernel/time/timer.c:2409
#13 handle_softirqs (...) at kernel/softirq.c:645
#14 __do_softirq () at kernel/softirq.c:679
#15 ____do_softirq (...) at arch/arm64/kernel/irq.c:78
#16 call_on_irq_stack () at arch/arm64/kernel/entry.S:885
#17 0x... in ?? ()
Backtrace stopped: previous frame inner to this frame (corrupt stack?)
```

The `timer_expire_remote`/`tmigr_*` frames are the NOHZ timer-migration
subsystem: this timer's *nominal* CPU was idle, so a different CPU's
softirq picked it up and is expiring it "remotely" on the idle CPU's
behalf rather than waking it just to run one timer — genuinely more of
the kernel's power-management machinery than the source comment alone
suggests, and a fair example of how a two-line "no sleeping in softirq
context" comment can sit on top of a much deeper real call chain. Two
things are also just artifacts, not bugs: several frames may be tagged
`[PAC]` (arm64 pointer authentication on the return address — GDB
strips it to unwind, and tags the frame so you know it did), and the
outermost frame commonly ends in `Backtrace stopped: previous frame
inner to this frame (corrupt stack?)` — that's GDB hitting the
irq-stack-switch boundary in `call_on_irq_stack`, not a real stack
corruption.

Confirm `current` here is whatever happened to be running when the
softirq fired, not anything meaningfully related to this timer. Plain
`current` is not visible as an ordinary C symbol to GDB over a remote
kernel target (it's backed by an architecture-specific per-CPU access,
not a global variable) — `print current->comm` fails with `No symbol
"current" in current context`. Use the `lx-symbols`-provided
convenience function instead, which reads it correctly:

```gdb
(gdb) print $lx_current().comm
(gdb) print $lx_current().pid
```

Live-tested, this printed `"swapper/0"` / `0` — the idle task, not
`insmod` or any process meaningfully related to this timer, confirming
the source comment's point directly.

Step through the actual work. **Exact `next` counts here are not
reliable** and shouldn't be memorized: `atomic64_inc()`/`atomic_set()`
on arm64 are commonly emitted through an alternative-patched LSE atomic
instruction sequence (see `arch/arm64/include/asm/alternative-macros.h`),
and a `next` from the function's opening line can land you *inside that
header* for a step before returning to `wait_queues_blocking.c` — the
same "next moves in the compiled sense, not the textual one" lesson
from lab 01, just a different concrete shape. Watch the line GDB
actually shows you after each `next` rather than counting how many
you've typed:

```gdb
(gdb) next    # may show alternative-macros.h briefly - keep going
(gdb) next
(gdb) print event_id   # confirm it incremented once you're back in this file, past the atomic64_inc line
(gdb) next     # atomic_set(&data_ready, 1)
(gdb) next      # wake_up_interruptible(&event_wq) - about to matter a lot in step 2
(gdb) next       # mod_timer() reschedules itself for the next interval
```

### Step 2 — catch a task actually blocked on the wait queue

This is the step this lab exists for. Delete the timer breakpoint (you
don't want it firing repeatedly while you set this up), and this time
break where a *reader* actually goes to sleep:

```gdb
(gdb) delete <the producer_fn breakpoint's number>
(gdb) break bq_read
(gdb) continue
```

(`delete` with no arguments deletes *everything*, but asks a `(y or n)`
confirmation first — worth avoiding here too, same reasoning as lab
01's cleanup section: name the number.)

```bash
# vmb:
cat /dev/blocking_demo &
```

The first hit is the call itself, before it's decided whether to
block:

```gdb
Thread 2 hit Breakpoint N, bq_read (...) at wait_queues_blocking.c:76
```

**Check for a pending event before going further — this is the step's
real trap.** By the time you reach this point you've typically already
spent a minute or more single-stepping through Step 1, and
`producer_fn` fires every `interval_ms` (1500ms by default) the whole
time, whether you're watching it or not. Live-tested running through
this walkthrough at a normal pace, `event_id` was already at `5` by the
time this breakpoint was reached — meaning `data_ready` is almost
certainly already `1` here, and `atomic_xchg(&data_ready, 0)` a few
lines down will take the *non-blocking* fast path and return
immediately, never touching `wait_event_interruptible()` at all. If you
don't check for this, the rest of this step simply won't show you what
it claims to: `waiter_count` never moves off `0`, `lx-ps` never shows
`cat` as blocked, and it's not obvious why.

```gdb
(gdb) print data_ready
```

If that reads `{counter = 1}` (expect it to, most of the time), this
first `cat` is about to return immediately without blocking — confirm
it, on purpose, as a real (if accidental) demonstration of the fast
path:

```gdb
(gdb) finish
```

This returns the already-available event straight away — genuinely
useful to see once, since it's the *other* branch through this
function's logic, but not what this step is for. `data_ready` is now
`0` again (the `atomic_xchg` cleared it on the way out). Start a fresh
read immediately, and get back to a breakpoint before the next tick has
a chance to set it again:

```bash
# vmb:
cat /dev/blocking_demo &
```
```gdb
(gdb) continue
```

You should land back in `bq_read` with `data_ready` genuinely `0` this
time (confirm with `print data_ready` again if you like). Now step
through for real:

```gdb
(gdb) next    # atomic_xchg(&data_ready, 0) - 0 this time, no event pending
(gdb) next     # not O_NONBLOCK, so: atomic_inc(&waiter_count)
```

Same caution as Step 1: GDB's `next` stops *before* executing the line
it displays, not after — so `waiter_count` is still `0` here, not `1`;
`print` it now and you'll see the pre-increment value:

```gdb
(gdb) print waiter_count
$1 = {counter = 0}
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
output at a breakpoint). If the intervening declaration lines
(`kbuf`/`len`/`ret`/`id`) appear to replay themselves under optimized
`-O2` code before you actually reach `wait_event_interruptible`, that's
the same DWARF-line-ordering looseness called out in Step 1, not a sign
you looped — `frame` will confirm you're still in the same call.

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

### Step 3 — wake it up, and watch the *pending* `next` resume on its own

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

**Don't type anything new into the `gdbsess` pane for this step — just
look at it.** The `next` you issued at the end of step 2 (on the
`wait_event_interruptible(...)` line) is still pending: `bq_read` never
returned to GDB's prompt, because that `next` is what's been sitting
inside `schedule()` this whole time. There is no fresh breakpoint hit
to wait for here — you already have a breakpoint on `bq_read`'s
*entry*, and this task never leaves the function, so entry is never
re-triggered. As soon as `wake_up_interruptible(&event_wq)` (called
from `trigger_store`, or from the next `producer_fn` tick if you didn't
beat the timer to it) makes the wait condition true, that outstanding
`next` simply completes on its own and GDB prints its result — switch
back to the `gdbsess` pane and it should already be sitting there:

```gdb
77          char kbuf[64];
```

(or wherever the DWARF line table happens to land, per the Step 1/2
caveat about optimized code) — the key point is you're *back*, in the
same call, with the same local variables, having crossed a real
sleep/wake/reschedule boundary without GDB losing track of anything.
Confirm the waiter count dropped back to `0` (`atomic_dec_return`
already ran on the way out of the wait) and step to where the loop
re-checks its condition:

```gdb
(gdb) print waiter_count
$N = {counter = 0}
```

If you land back at the top of `bq_read`'s `for (;;)` loop, this is
correct: `wait_event_interruptible()` returning doesn't mean you're
past the function, it means the loop's condition check runs again,
this time finding `atomic_xchg(&data_ready, 0)` true and `break`ing out
for real:

```gdb
(gdb) next
(gdb) print id
```

### Step 4 — `poll()`: readiness without blocking

```gdb
(gdb) delete <the bq_read breakpoint's number>
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

**`break wait_queues_blocking_exit` does not work here — confirmed
live, and worth understanding why, because it affects every lab whose
cleanup function uses the modern `module_exit()` macro (i.e. every lab
in this repo except 01).** `wait_queues_blocking_exit` is marked
`__exit`, which places it in its own ELF section, `.exit.text`,
separate from the module's regular `.text`. `lx-symbols` relocates only
a fixed, hardcoded list of extra sections when it maps a loaded
module's addresses — in this kernel's
`scripts/gdb/linux/symbols.py`, that list (`_section_arguments()`) is
literally `.data`, `.data..read_mostly`, `.rodata`, `.bss`,
`.text.hot`, `.text.unlikely`, plus the base `.text`. `.exit.text`
(and `.init.text`) are simply never in it. So `break
wait_queues_blocking_exit` — or `break wait_queues_blocking.c:222` by
line, same result — silently resolves to the function's *raw,
unrelocated* file offset (something tiny like `0x170`, not a real
kernel address), and setting it appears to succeed with no error. It
just never fires. `rmmod` will complete normally, dmesg will show the
module's own unload message, and GDB will simply sit at `Continuing.`
forever, having quietly missed it — there is no error message pointing
at the real cause.

**Diagnose this directly if you hit it**: compare the address GDB
reports in `info breakpoints` against the real one from the guest's
own sysfs:

```bash
# vmb:
cat /sys/module/wait_queues_blocking/sections/.exit.text
```

If that's a real `0xffff8...`-style kernel address and GDB's `info
breakpoints` shows something tiny by comparison, you've hit exactly
this.

**The fix that works, verified live** — break on the generic kernel
hook that calls into every module's exit function, the same pattern
Step 1 uses for init via `do_init_module`:

```gdb
(gdb) break __do_sys_delete_module
(gdb) continue
```
```bash
# vmb (make sure no backgrounded cat still holds the device open first -
# see the reference-count note below):
rmmod wait_queues_blocking
```

Advance to the actual call site (check with `list` if this line number
has drifted on a different kernel version — you want the `mod->exit();`
line inside the `delete_module` syscall handler in
`kernel/module/main.c`) and read the real address straight out of the
kernel's own struct, bypassing GDB's broken section table entirely:

```gdb
(gdb) advance kernel/module/main.c:863
(gdb) print mod->exit
$N = (void (*)(void)) 0xffff80007c320640
```

(That exact address is from one real run and will not match yours —
module memory is allocated fresh each boot regardless of `nokaslr`,
which only fixes the *kernel image's* own load address, not per-module
placement. Always use whatever `print mod->exit` gives you right now,
not the number above — every address quoted for the rest of this
section is illustrative of that same one run, for the same reason.)

**Do not `step` into it from here** — confirmed live, this is a real
trap, not a hypothetical one: since `.exit.text` has no relocated
symbol table entry, GDB can't determine "the bounds of the current
function" once it's inside it, so `step` never finds a recognized
line to stop on. It just keeps single-instruction-stepping through
everything from that point forward — the rest of the module's exit
function, then whatever `__do_sys_delete_module` does next, and beyond
— for as long as you let it. In this repo's own test it ran all the way
through to the CPU's idle loop before being interrupted, with `rmmod`
having long since completed underneath it. If you've already typed
`step` here, `Ctrl-C` gets you back (same guest-freeze/interrupt
behavior as anywhere else in KGDB), and no harm is done — just delete
whatever you get and start again from `print mod->exit` above.

Instead, break directly at that address:

```gdb
(gdb) break *0xffff80007c320640
(gdb) continue
```

This *does* stop exactly where you want — but GDB shows it as `?? ()`
with no name, and `next`/`bt` won't work there either, for the same
underlying reason. For a fully working breakpoint (correct name,
`next`/`bt`/`finish` all functional for anything it calls into),
register the section's real address with GDB the same way
`lx-symbols` itself does it for the sections it already knows about:

```gdb
(gdb) delete <the raw-address breakpoint's number>
(gdb) add-symbol-file /home/adiopocere/Desktop/codes/linux-kernel-project/12_wait_queues_blocking/wait_queues_blocking.ko -s .exit.text 0xffff80007c320640
```

(confirm the `(y or n)` prompt with `y` — GDB is just noting the file's
already loaded and asking if you really want to add this extra section
mapping on top). Now:

```gdb
(gdb) break wait_queues_blocking_exit
Breakpoint N at 0x170: wait_queues_blocking_exit. (2 locations)
```

Two locations: `N.1` is still the old broken raw-offset one, `N.2` is
the newly-relocated one — disable the first, keep the second:

```gdb
(gdb) disable N.1
(gdb) continue
```
```bash
# vmb:
rmmod wait_queues_blocking
```

This hits cleanly, with a real name — live-tested, it reported as
`cleanup_module` rather than `wait_queues_blocking_exit`: the
`module_exit()` macro aliases the function to the legacy `cleanup_module`
symbol name too, the exact same mechanism lab 01/02 already cover for
`init_module`.

```gdb
Thread N hit Breakpoint N.2, 0xffff80007c320644 in cleanup_module ()
```

Read the source's own comment on the next line before stepping past
it: it explains that `rmmod` can never actually reach here while a
reader is genuinely blocked in `bq_read()`, because `fops.owner =
THIS_MODULE` means every open fd holds a module reference for its
whole lifetime, and `rmmod` refuses to unload a module with a nonzero
reference count. If you still have a backgrounded `cat` from step 2/3
holding the device open, `rmmod` will simply fail with "in use" rather
than this exit path ever running concurrently with a blocked reader —
confirm it yourself: `kill` the backgrounded `cat` first, *then*
`rmmod`.

`next` inside `cleanup_module` itself won't have line-by-line
resolution — the section relocation you just added fixes symbol *and*
breakpoint addresses, but not the DWARF line table for code inside it,
so GDB will tell you it's "single stepping until exit from function
cleanup_module, which has no line number information." That's fine:
`next` still correctly runs until it lands in whatever real,
fully-resolved vmlinux function this code calls next (here,
`timer_shutdown_sync`), which has full debug info as normal:

```gdb
(gdb) next
timer_shutdown_sync (timer=0x... <producer_timer>) at kernel/time/timer.c:1717
(gdb) finish
Value returned is $N = 1
```

`timer_shutdown_sync()`'s return value matters: `1` here means a
pending/active timer actually existed and was cancelled by this call
(0 would mean it had already fired and there was nothing to cancel) —
directly confirms the ordering the source comment warns about: this
runs *before* the module's data is torn down, specifically so a
still-in-flight `producer_fn()` can't race the unload.

```gdb
(gdb) continue
```
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
