# GDB walkthrough — 15_kthreads

`kthreads.c` spawns a genuine, dedicated kernel thread with
`kthread_run()` — unlike module 03's workqueue callback or module 14's timer,
this producer has its own persistent `task_struct`, its own PID, and
its own call stack that exists continuously across many loop
iterations, not just for the duration of one callback. The debugging
angle: finding this thread among every other task in the system with
`lx-ps`, breaking inside its loop body, and watching
`kthread_stop()`'s cooperative shutdown protocol actually unblock a
sleeping thread rather than killing it.

## Environment

```bash
cd 15_kthreads
make -C /home/adiopocere/Desktop/codes/linux_mainline M=$(pwd) modules
modinfo kthreads.ko | grep vermagic
sudo mount -o loop /home/adiopocere/Desktop/codes/qemu-vmb/labs-disk.img /tmp/vmb-mnt
sudo mkdir -p /tmp/vmb-mnt/15_kthreads
sudo cp kthreads.ko /tmp/vmb-mnt/15_kthreads/
sudo umount /tmp/vmb-mnt
```

## tmux layout

Standard `vmb` + `gdbsess` — [`../gdb_debugging.md`](../gdb_debugging.md).

## Real, verified breakpoint targets

```
Line 64:  producer_thread_fn
Line 100: start_producer
Line 122: stop_producer
Line 154: kthread_demo_read
Line 270: kthreads_init
Line 306: kthreads_exit
```

Note: `kthreads_init()` calls `start_producer()` itself during load, so
the thread is already running by the time `insmod` returns — there is
no separate manual "start" step required before you can find it.

## The walkthrough

### Step 1 — load, then find the thread among every task in the system

```gdb
(gdb) target remote :1234
(gdb) lx-version
(gdb) break do_init_module
(gdb) continue
```
```bash
# vmb:
insmod /mnt/labs/15_kthreads/kthreads.ko interval_ms=1000
```
```gdb
(gdb) lx-symbols /home/adiopocere/Desktop/codes/linux-kernel-project
(gdb) lx-ps
```

Scan the output for `kthread_demo_producer` — the exact name
`kthread_run(producer_thread_fn, NULL, "kthread_demo_producer")`
registered it under. This confirms the thread genuinely exists as a
schedulable task, distinct from every `kworker`, `insmod`, and your own
shell, with its own PID:

```bash
# vmb, cross-check from userspace:
cat /sys/kernel/kthreads_demo/status
```

`status`'s `pid=` field should match the PID `lx-ps` just showed you —
same thread, two different ways of asking the kernel who it is.

### Step 2 — break inside the loop body while it's genuinely running

```gdb
(gdb) break producer_thread_fn
(gdb) continue
```

Nothing to do in `vmb` — within a second (or `interval_ms`, whichever
is shorter):

```gdb
Thread 2 hit Breakpoint N, producer_thread_fn (data=0x0) at kthreads.c:64
```

Wait — this only fires once, at the very top, the *first* time through
before the loop even starts (the `pr_info("producer thread started")`
line runs once, above the `while` loop). Set a breakpoint *inside* the
loop body specifically to catch every iteration:

```gdb
(gdb) delete <the producer_thread_fn breakpoint's number — `info breakpoints` if unsure>
(gdb) break kthreads.c:71
(gdb) continue
```

(Bare `delete` with no argument deletes *every* breakpoint, but first
asks `Delete all breakpoints? (y or n)` — if you're typing ahead, that
prompt can silently swallow your next command instead of actually
deleting anything. Naming the number skips the prompt; same reasoning
applies to every `delete` in the rest of this walkthrough.)

(Line 71 is `ring[ring_head].seq = next_seq++;` — the first real line
inside `while (!kthread_should_stop())`; verify it matches your build
with `list 68,75` once stopped, since exact line numbers only ever
come from this specific compiled source, not from memory.)

```gdb
Thread 2 hit Breakpoint N, producer_thread_fn (data=0x0) at kthreads.c:71
(gdb) print current->comm
$1 = "kthread_demo_pro\000..."     # TASK_COMM_LEN truncates long names - see it get cut off right here
(gdb) print current->pid
```

This confirms `current` really is this thread's own task, persistently
— not, as in modules 03/14, whatever happened to be running when some
generic callback fired. Compare `current->comm`'s truncation against
the full `"kthread_demo_producer"` string you searched for in `lx-ps`
a moment ago: `TASK_COMM_LEN` (16 bytes) cuts it short in
`current->comm` itself — a real, generic kernel limit, not specific to
this driver.

```gdb
(gdb) next    # mutex_lock(&ring_lock)
(gdb) next     # ring[ring_head].seq = next_seq++
(gdb) print next_seq
(gdb) next      # ring[ring_head].ns = ktime_get_ns()
(gdb) next       # ring_head advance
(gdb) print ring_head
(gdb) next        # ring_count bookkeeping
(gdb) next         # mutex_unlock
(gdb) next          # msleep_interruptible(interval_ms) - this thread sleeps here
```

### Step 3 — catch it asleep, then watch `kthread_stop()` wake it early

`continue` once (letting it finish the `msleep_interruptible` and loop
back around) to confirm the breakpoint fires repeatedly — this is your
producer, ticking on its own schedule, same as any timer/workqueue,
except this time you can watch `current` stay the *same* PID across
every single hit, iteration after iteration:

```gdb
(gdb) continue
Thread 2 hit Breakpoint N, producer_thread_fn (...) at kthreads.c:71
(gdb) print current->pid       # identical to step 2's value
```

Now delete this breakpoint, let the thread go to sleep in
`msleep_interruptible()`, and catch it *there* instead:

```gdb
(gdb) delete <the kthreads.c:71 breakpoint's number>
```

Set `interval_ms` high first so you have time to act before it wakes
on its own:

```bash
# vmb:
echo stop | tee /sys/kernel/kthreads_demo/control    # actually don't - read on first
```

Don't run that yet — instead, confirm the thread is genuinely asleep
right now with `lx-ps` (its state should read `INTERRUPTIBLE`, the
same state module 12's blocked reader showed), *then* trigger the stop:

```gdb
(gdb) lx-ps
```
```bash
# vmb:
echo stop | tee /sys/kernel/kthreads_demo/control
```
```gdb
(gdb) break kthread_should_stop
(gdb) continue
```

You will land inside generic kernel code (`kernel/kthread.c`), not
this driver — `kthread_should_stop()` is what the `while` loop's
condition actually calls every iteration, and it's the *return*
becoming true, right after `kthread_stop()` (called from
`stop_producer()`, itself triggered by your `control` write) both
flags the thread to stop **and** wakes it if it was sleeping:

```gdb
(gdb) bt
#0  kthread_should_stop () at kernel/kthread.c:...
#1  0x... in producer_thread_fn (data=0x0) at kthreads.c:68
```

Continue and watch the loop actually exit rather than sleep out its
full remaining interval:

```gdb
(gdb) finish
(gdb) continue
```
```bash
# vmb:
cat /sys/kernel/kthreads_demo/status
```
`running=0`, `pid=-1` — confirmed the thread is genuinely gone, not
just marked for future cleanup.

### Step 4 — `stop_producer`/`start_producer`: the handoff itself

```gdb
(gdb) delete <the kthread_should_stop breakpoint's number>
(gdb) break stop_producer
(gdb) break start_producer
```

Verified: `Line 100 <start_producer>`, `Line 122 <stop_producer>`.
Restart it and watch the `producer_task` pointer get set:

```bash
# vmb:
echo start | tee /sys/kernel/kthreads_demo/control
```
```gdb
(gdb) continue
Thread 2 hit Breakpoint N, start_producer () at kthreads.c:100
(gdb) next   # mutex_lock(&producer_task_lock)
(gdb) next    # `if (producer_task)` - false, since step 3 stopped it
(gdb) next     # kthread_run(...)
(gdb) print producer_task
$2 = (struct task_struct *) 0x...
(gdb) print producer_task->pid
```

A **new** PID from step 2/3's thread — `kthread_stop()` doesn't leave
anything reusable behind; a fresh `kthread_run()` genuinely creates a
new task.

### Step 5 — `kthread_demo_read`: drain-on-read, not blocking

```gdb
(gdb) delete <the stop_producer and start_producer breakpoints' numbers>
(gdb) break kthread_demo_read
(gdb) continue
```
```bash
# vmb:
cat /dev/kthread_demo
```
```gdb
Thread 2 hit Breakpoint N, kthread_demo_read (...) at kthreads.c:154
(gdb) next   # `if (*ppos == 0)` - only drains the ring on a fresh read from offset 0
(gdb) next    # mutex_lock(&ring_lock)
(gdb) next     # n = ring_count; start = ... - the ring buffer's wraparound math
(gdb) print n
(gdb) print start
```

Unlike module 12's `bq_read()`, this never calls anything that sleeps —
confirm by comparing `lx-ps`'s state for whatever process is running
`cat` right now against module 12's blocked reader: this one stays
`RUNNING` the whole time, because "nothing buffered" here just returns
0 bytes immediately (real EOF-like behavior) rather than waiting for
more to arrive.

## Cleanup

**`break kthreads_exit` does not work if you try it directly —
confirmed live.** `kthreads_exit` is marked `__exit`, placing it in its
own ELF section, `.exit.text`, which `lx-symbols` never relocates (its
hardcoded section list in `scripts/gdb/linux/symbols.py` doesn't
include `.init.text`/`.exit.text`). The breakpoint silently resolves to
a raw, unrelocated file offset instead of a real address — no error, it
just never fires. This affects every module in this repo using the
modern `module_exit()` macro (every module except 01).

**The fix, verified live** — break on the generic kernel hook that
calls into every module's exit function, then read the real address
out of the kernel's own struct:

```gdb
(gdb) delete <the kthread_demo_read breakpoint's number>
(gdb) break __do_sys_delete_module
(gdb) continue
```
```bash
# vmb:
rmmod kthreads
```
```gdb
(gdb) advance kernel/module/main.c:863
(gdb) print mod->exit
$N = (void (*)(void)) 0xffff80007c320870
```

(That address is from one real run and won't match yours — module
memory placement is random per boot regardless of `nokaslr`. Always use
whatever `print mod->exit` gives you right now.) **Do not `step` into
it from here** — with no relocated line table GDB can't bound the
function and `step` free-runs straight past it; `Ctrl-C` recovers you.
Register the section the way `lx-symbols` does for the sections it
already knows about, and the normal breakpoint then resolves cleanly:

```gdb
(gdb) add-symbol-file /home/adiopocere/Desktop/codes/linux-kernel-project/15_kthreads/kthreads.ko -s .exit.text 0xffff80007c320870
(gdb) break kthreads_exit
Breakpoint N at 0xffff80007c320870: file kthreads.c, line 306.
(gdb) delete <the __do_sys_delete_module breakpoint's number>
(gdb) continue
```
```bash
# vmb:
rmmod kthreads
```
```gdb
Thread N hit Breakpoint N, kthreads_exit () at kthreads.c:306
306		stop_producer();
(gdb) next   # stop_producer() - same cooperative shutdown as step 3, now via module exit
(gdb) continue
```
```bash
# vmb:
poweroff -f
```

## What this proves

A kernel thread is not a callback firing on borrowed context — it's a
persistent, independently schedulable task with its own PID and its
own perpetually-the-same `current`, visible directly in `lx-ps` and
confirmed by watching that PID stay identical across every breakpoint
hit inside its loop. `kthread_stop()`'s two-part contract
(flag-then-wake) is the specific mechanism `msleep_interruptible()`
relies on to make shutdown *prompt* rather than making the caller wait
out a full sleep interval — caught here as a literal
`kthread_should_stop()` call returning true immediately after
`kthread_stop()` runs, not merely asserted by the source comment.
