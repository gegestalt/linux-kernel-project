# GDB walkthrough — 15_kthreads, hands-on, start to finish

`kthreads.c` spawns a genuine, dedicated kernel thread with
`kthread_run()` — unlike module 03's workqueue callback or module 14's
timer, this producer has its own persistent `task_struct`, its own PID,
and its own call stack that exists continuously across many loop
iterations, not just for the duration of one callback. The debugging
angle: finding this thread among every other task in the system with
`lx-ps`, breaking inside its loop body, and watching `kthread_stop()`'s
cooperative shutdown protocol actually unblock a sleeping thread rather
than killing it.

Every command below says exactly which pane. One command per step,
always — paste it, wait for the prompt to come back, then the next one.
`kthreads_init()` calls `start_producer()` itself during load, so the
thread is already running by the time `insmod` returns — no separate
manual "start" step is needed before you can find it.

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

## Step 1 — build it

*Regular terminal (detach with `Ctrl-b d`, or a separate window).*

```bash
cd /home/adiopocere/Desktop/codes/linux-kernel-project/15_kthreads
make -C /home/adiopocere/Desktop/codes/linux_mainline M=$(pwd) modules
```

## Step 2 — check vermagic, copy onto the scratch disk

```bash
modinfo kthreads.ko | grep vermagic
```
```bash
sudo mount -o loop /home/adiopocere/Desktop/codes/qemu-vmb/labs-disk.img /tmp/vmb-mnt
sudo mkdir -p /tmp/vmb-mnt/15_kthreads
sudo cp kthreads.ko /tmp/vmb-mnt/15_kthreads/
sudo umount /tmp/vmb-mnt
```

## Step 3 — boot the guest

**Pane: vmb**

```bash
qemu-system-aarch64 -M virt -cpu max -m 1024 -smp 2 \
  -kernel /home/adiopocere/Desktop/codes/linux_mainline/arch/arm64/boot/Image \
  -initrd /home/adiopocere/Desktop/codes/qemu-vmb/initramfs.cpio.gz \
  -drive file=/home/adiopocere/Desktop/codes/qemu-vmb/labs-disk.img,if=virtio,format=raw \
  -append "console=ttyAMA0 rdinit=/init nokaslr" -nographic -s
```

Wait for `=== VM B (QEMU) ready ===` and `~ #`.

## Step 4 — start gdb, connect

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

## Step 5 — break on the load entry point

**Pane: gdb**

```
break do_init_module
```
```
continue
```

Prints `Continuing.` — switch panes.

## Step 6 — load, then find the thread among every task in the system

**Pane: vmb**

```bash
insmod /mnt/labs/15_kthreads/kthreads.ko interval_ms=1000
```

**Pane: gdb**

```
Thread 2 hit Breakpoint 1, do_init_module (mod=mod@entry=0x...) at kernel/module/main.c:3089
```
```
lx-symbols /home/adiopocere/Desktop/codes/linux-kernel-project
```
```
lx-ps
```

**What this shows:** scan the output for `kthread_demo_producer` — the
exact name `kthread_run(producer_thread_fn, NULL, "kthread_demo_producer")`
registered it under. This confirms the thread genuinely exists as a
schedulable task, distinct from every `kworker`, `insmod`, and your own
shell, with its own PID.

**Pane: vmb**

```bash
cat /sys/kernel/kthreads_demo/status
```

**What this shows:** `status`'s `pid=` field should match the PID
`lx-ps` just showed you — same thread, two different ways of asking the
kernel who it is.

## Step 7 — break inside the loop body while it's genuinely running

**Pane: gdb**

```
break producer_thread_fn
```
```
continue
```

Nothing to do in `vmb` — within a second (or `interval_ms`, whichever is
shorter):

```
Thread 2 hit Breakpoint 2, producer_thread_fn (data=0x0) at kthreads.c:64
```

**What this shows:** wait — this only fires once, at the very top, the
*first* time through before the loop even starts (the
`pr_info("producer thread started")` line runs once, above the `while`
loop). Set a breakpoint *inside* the loop body specifically to catch
every iteration:

```
delete
```
```
y
```

(Bare `delete` with no argument deletes *every* breakpoint, but first
asks `Delete all breakpoints? (y or n)` — naming a specific number
instead skips the prompt if you'd rather keep others armed. Same
reasoning applies to every `delete` in the rest of this walkthrough.)

```
break kthreads.c:71
```
```
continue
```

(Line 71 is `ring[ring_head].seq = next_seq++;` — the first real line
inside `while (!kthread_should_stop())`; verify it matches your build
with `list 68,75` once stopped, since exact line numbers only ever come
from this specific compiled source, not from memory.)

```
Thread 2 hit Breakpoint 3, producer_thread_fn (data=0x0) at kthreads.c:71
```
```
print current->comm
```
```
$1 = "kthread_demo_pro\000..."
```

**What this shows:** `TASK_COMM_LEN` truncates long names — see it get
cut off right here. Compare this truncation against the full
`"kthread_demo_producer"` string you searched for in `lx-ps` a moment
ago: `TASK_COMM_LEN` (16 bytes) cuts it short in `current->comm` itself
— a real, generic kernel limit, not specific to this driver.

```
print current->pid
```

**What this shows:** this confirms `current` really is this thread's own
task, persistently — not, as in modules 03/14, whatever happened to be
running when some generic callback fired.

```
next
```

(`mutex_lock(&ring_lock)`.)

```
next
```

(`ring[ring_head].seq = next_seq++`.)

```
print next_seq
```
```
next
```

(`ring[ring_head].ns = ktime_get_ns()`.)

```
next
```

(`ring_head` advance.)

```
print ring_head
```
```
next
```

(`ring_count` bookkeeping.)

```
next
```

(`mutex_unlock`.)

```
next
```

(`msleep_interruptible(interval_ms)` — this thread sleeps here.)

## Step 8 — catch it asleep, then watch `kthread_stop()` wake it early

**Pane: gdb**

```
continue
```
```
Thread 2 hit Breakpoint 3, producer_thread_fn (...) at kthreads.c:71
```
```
print current->pid
```

**What this shows:** identical to step 7's value — this is your
producer, ticking on its own schedule, same as any timer/workqueue,
except this time `current` stays the *same* PID across every single
hit, iteration after iteration.

```
delete
```
```
y
```
```
lx-ps
```

**What this shows:** confirm the thread is genuinely asleep right now —
its state should read `INTERRUPTIBLE`, the same state module 12's
blocked reader showed.

**Pane: vmb**

```bash
echo stop | tee /sys/kernel/kthreads_demo/control
```

**Pane: gdb**

```
break kthread_should_stop
```
```
continue
```

**What this shows:** you land inside generic kernel code
(`kernel/kthread.c`), not this driver — `kthread_should_stop()` is what
the `while` loop's condition actually calls every iteration, and it's
the *return* becoming true, right after `kthread_stop()` (called from
`stop_producer()`, itself triggered by your `control` write) both flags
the thread to stop **and** wakes it if it was sleeping.

```
bt
```
```
#0  kthread_should_stop () at kernel/kthread.c:...
#1  0x... in producer_thread_fn (data=0x0) at kthreads.c:68
```
```
finish
```
```
continue
```

**Pane: vmb**

```bash
cat /sys/kernel/kthreads_demo/status
```

**What this shows:** `running=0`, `pid=-1` — confirmed the thread is
genuinely gone, watched the loop actually exit rather than sleep out its
full remaining interval.

## Step 9 — `stop_producer`/`start_producer`: the handoff itself

**Pane: gdb**

```
delete
```
```
y
```
```
break stop_producer
```
```
break start_producer
```

**Pane: vmb**

```bash
echo start | tee /sys/kernel/kthreads_demo/control
```

**Pane: gdb**

```
continue
```
```
Thread 2 hit Breakpoint 5, start_producer () at kthreads.c:100
```
```
next
```

(`mutex_lock(&producer_task_lock)`.)

```
next
```

(`if (producer_task)` — false, since step 8 stopped it.)

```
next
```

(`kthread_run(...)`.)

```
print producer_task
```
```
$2 = (struct task_struct *) 0x...
```
```
print producer_task->pid
```

**What this shows:** a **new** PID from step 7/8's thread —
`kthread_stop()` doesn't leave anything reusable behind; a fresh
`kthread_run()` genuinely creates a new task.

## Step 10 — `kthread_demo_read`: drain-on-read, not blocking

**Pane: gdb**

```
delete
```
```
y
```
```
break kthread_demo_read
```
```
continue
```

**Pane: vmb**

```bash
cat /dev/kthread_demo
```

**Pane: gdb**

```
Thread 2 hit Breakpoint 6, kthread_demo_read (...) at kthreads.c:154
```
```
next
```

(`if (*ppos == 0)` — only drains the ring on a fresh read from offset 0.)

```
next
```

(`mutex_lock(&ring_lock)`.)

```
next
```

(`n = ring_count; start = ...` — the ring buffer's wraparound math.)

```
print n
```
```
print start
```

**What this shows:** unlike module 12's `bq_read()`, this never calls
anything that sleeps — this task stays `RUNNING` the whole time (check
with `lx-ps` if you like), because "nothing buffered" here just returns
0 bytes immediately (real EOF-like behavior) rather than waiting for
more to arrive.

## Step 11 — the exit path: `__exit`-section relocation gotcha

`kthreads_exit` is marked `__exit`, placing it in its own ELF section,
`.exit.text`, which `lx-symbols` never relocates (its hardcoded section
list in `scripts/gdb/linux/symbols.py` doesn't include `.init.text`/
`.exit.text`). A direct `break kthreads_exit` right now would silently
resolve to a raw, unrelocated file offset — no error, it just never
fires. This affects every module in this repo using the modern
`module_exit()` macro (every module except 01).

**Pane: gdb**

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
rmmod kthreads
```

**Pane: gdb**

```
advance kernel/module/main.c:863
```
```
print mod->exit
```
```
$3 = (void (*)(void)) 0xffff80007c320870
```

(That address is from one real run and won't match yours — module
memory placement is random per boot regardless of `nokaslr`. Always use
whatever `print mod->exit` gives you right now. **Do not `step` into it
from here** — with no relocated line table GDB can't bound the function
and `step` free-runs straight past it; `Ctrl-C` recovers you.)

```
add-symbol-file /home/adiopocere/Desktop/codes/linux-kernel-project/15_kthreads/kthreads.ko -s .exit.text 0xffff80007c320870
```
```
y
```
```
break kthreads_exit
```
```
Breakpoint 8 at 0xffff80007c320870: file kthreads.c, line 306.
```
```
delete
```
```
y
```
```
continue
```

**Pane: vmb**

```bash
rmmod kthreads
```

**Pane: gdb**

```
Thread 2 hit Breakpoint 8, kthreads_exit () at kthreads.c:306
306		stop_producer();
```
```
next
```

**What this shows:** `stop_producer()` — the same cooperative shutdown
as step 8, now triggered via module exit instead of the sysfs `control`
file.

```
continue
```

## Step 12 — clean up

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

A kernel thread is not a callback firing on borrowed context — it's a
persistent, independently schedulable task with its own PID and its own
perpetually-the-same `current`, visible directly in `lx-ps` and
confirmed by watching that PID stay identical across every breakpoint
hit inside its loop (step 7, step 8). `kthread_stop()`'s two-part
contract (flag-then-wake) is the specific mechanism
`msleep_interruptible()` relies on to make shutdown *prompt* rather than
making the caller wait out a full sleep interval — caught here as a
literal `kthread_should_stop()` call returning true immediately after
`kthread_stop()` runs, not merely asserted by the source comment.
