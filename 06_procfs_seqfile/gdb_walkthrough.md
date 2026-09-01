# GDB walkthrough — 06_procfs_seqfile, hands-on, start to finish

`procfs_seqfile.c` implements the full `seq_file` iterator contract —
the same `start()`/`next()`/`show()`/`stop()` protocol the kernel uses
internally for things like `/proc/PID/maps` — plus a simpler
single-value file via `proc_create_single()`. This walkthrough's job is
to make the four-callback contract *observable*: breaking on all four at
once and watching the exact call sequence a single
`cat /proc/procfs_demo/events` produces, including the one detail that's
easy to miss reading the source alone — `stop()` runs even on the call
where `start()` returned `NULL`.

Every command below says exactly which pane. One command per step,
always — paste it, wait for the prompt to come back, then the next one.

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
cd /home/adiopocere/Desktop/codes/linux-kernel-project/06_procfs_seqfile
make -C /home/adiopocere/Desktop/codes/linux_mainline M=$(pwd) modules
```

## Step 2 — confirm the breakpoint targets, statically, before touching a VM

```bash
gdb -q -batch -nx -ex "file procfs_seqfile.ko" \
    -ex "info line info_show" -ex "info line events_write" \
    -ex "info line record_event" -ex "ptype struct proc_event" procfs_seqfile.ko
```
```
Line 79 of "procfs_seqfile.c" starts at address 0x4e8 <info_show> ...
Line 171 of "procfs_seqfile.c" starts at address 0x5d0 <events_write> ...
Line 53 of "procfs_seqfile.c" starts at address 0x364 <events_open+28> ...   # inlined!
type = struct proc_event {
    u64 ns;
    pid_t pid;
    char comm[16];
}
```

**`record_event` has no symbol of its own** — small, single-call-site,
GCC inlined it entirely into `events_open` (the address lands at
`events_open+28`, not a standalone function). Break on `events_open`
instead if you want to catch it. `events_seq_start`/`next`/`show`/`stop`
and `info_show` all resolve to real, independent symbols — confirmed
above and in the walkthrough itself below.

## Step 3 — check vermagic, copy onto the scratch disk

```bash
modinfo procfs_seqfile.ko | grep vermagic
```
```bash
sudo mount -o loop /home/adiopocere/Desktop/codes/qemu-vmb/labs-disk.img /tmp/vmb-mnt
sudo mkdir -p /tmp/vmb-mnt/06_procfs_seqfile
sudo cp procfs_seqfile.ko /tmp/vmb-mnt/06_procfs_seqfile/
sudo umount /tmp/vmb-mnt
```

## Step 4 — boot the guest

**Pane: vmb**

```bash
qemu-system-aarch64 -M virt -cpu max -m 1024 -smp 2 \
  -kernel /home/adiopocere/Desktop/codes/linux_mainline/arch/arm64/boot/Image \
  -initrd /home/adiopocere/Desktop/codes/qemu-vmb/initramfs.cpio.gz \
  -drive file=/home/adiopocere/Desktop/codes/qemu-vmb/labs-disk.img,if=virtio,format=raw \
  -append "console=ttyAMA0 rdinit=/init nokaslr" -nographic -s
```

Wait for `=== VM B (QEMU) ready ===` and `~ #`.

## Step 5 — start gdb, connect, load the module

**Pane: gdb**

```bash
cd /home/adiopocere/Desktop/codes/linux_mainline && gdb -q -iex 'set auto-load safe-path /' vmlinux
```
```
target remote :1234
```
```
break do_init_module
```
```
continue
```

Prints `Continuing.` — switch panes.

**Pane: vmb**

```bash
insmod /mnt/labs/06_procfs_seqfile/procfs_seqfile.ko
```

**Pane: gdb**

```
lx-symbols /home/adiopocere/Desktop/codes/linux-kernel-project
```

## Step 6 — get some events into the log first

`events_seq_start`/`next` only walk a real, non-empty array if something
has opened `/proc/procfs_demo/events` before now — generate a few
entries before setting the seq_file breakpoints, so the next step has
actual multi-element iteration to show, not a single-row edge case.

**Pane: vmb**

```bash
cat /proc/procfs_demo/events > /dev/null
```
```bash
cat /proc/procfs_demo/events > /dev/null
```
```bash
cat /proc/procfs_demo/events > /dev/null
```

## Step 7 — arm all four seq_file callbacks

**Pane: gdb**

```
break events_seq_start
```
```
break events_seq_next
```
```
break events_seq_show
```
```
break events_seq_stop
```
```
continue
```

**Pane: vmb**

```bash
cat /proc/procfs_demo/events
```

## Step 8 — walk the four-callback sequence, one hit at a time

**Pane: gdb**

```
Thread 2 hit Breakpoint 1, events_seq_start (s=0x..., pos=0x...) at procfs_seqfile.c:120
```
```
print *pos
```
```
$1 = 0
```
```
continue
```
```
Thread 2 hit Breakpoint 3, events_seq_show (s=0x..., v=0x...) at procfs_seqfile.c:145
```
```
print ((struct proc_event *)v)->pid
```
```
continue
```
```
Thread 2 hit Breakpoint 2, events_seq_next (s=0x..., v=0x..., pos=0x...) at procfs_seqfile.c:130
```
```
print *pos
```
```
$2 = 0
```

**Not yet incremented** — `next`'s own `(*pos)++` hasn't executed at
line 130 yet, it's the very first statement inside the function body.

```
next
```
```
print *pos
```
```
$3 = 1
```
```
continue
```

Keep repeating `continue` through the `show`/`next` pairs — one pair per
logged event (this module's 3 opens from step 6 plus this `cat`'s own
open makes 4 total). On the last iteration, `events_seq_next` sets `*pos`
past `event_count` and returns `NULL`, which is what ends the loop —
catch it directly on the final hit:

```
next
```
```
print *pos
```
```
print event_count
```

Once `*pos >= event_count`:

```
finish
```
```
Value returned is $4 = (void *) 0x0
```
```
continue
```
```
Thread 2 hit Breakpoint 4, events_seq_stop (s=0x..., v=0x...) at procfs_seqfile.c:140
```
```
print v
```
```
$5 = (void *) 0x0
```

**This is the detail worth stopping on.** `events_seq_stop` just fired
with `v == NULL` — the exact value `events_seq_next` returned to signal
"no more elements." The source comment in `procfs_seqfile.c` states this
("stop() is always called to match start(), even on error paths and even
when start() itself returned NULL") as a documentation claim; you've now
watched it actually happen. It's why `events_lock` is taken in `start()`
and dropped in `stop()` rather than per-element in `show()` — `stop()` is
the one callback in this protocol guaranteed to run regardless of how the
iteration ended.

## Step 9 — `info_show`: the simpler single-value file

**Pane: gdb**

```
info breakpoints
```

Note the numbers of the four `events_seq_*` breakpoints, then delete
them by number — bare `delete` prompts for confirmation, and a queued-up
next command can eat that prompt instead of actually deleting anything:

```
delete 1 2 3 4
```
```
break info_show
```
```
continue
```

**Pane: vmb**

```bash
cat /proc/procfs_demo/info
```

**Pane: gdb**

```
Thread 2 hit Breakpoint N, info_show (s=0x..., v=0x...) at procfs_seqfile.c:79
```
```
next
```
```
print uptime_ms
```
```
next
```
```
print total_opens
```

Compare `total_opens` here against `event_count` from step 8 —
`total_opens` increments on *every* open of `events` (step 6's three
plus step 7's one, so 4), while `event_count` only grows up to
`MAX_EVENTS` (64) before the array itself stops accepting new records
even though `total_opens` keeps counting — the exact bounded-log
behavior the source comment describes, now backed by two numbers read
directly out of memory.

## Step 10 — the write path: clearing the log

**Pane: gdb**

```
info breakpoints
```
```
delete N
```

(Use the actual `info_show` breakpoint number from step 9.)

```
break events_write
```
```
continue
```

**Pane: vmb**

```bash
echo clear > /proc/procfs_demo/events
```

**Pane: gdb**

```
Thread 2 hit Breakpoint N, events_write (...) at procfs_seqfile.c:169
```
```
next
```
```
print kbuf
```
```
next
```
```
next
```
```
print event_count
```
```
$6 = 0
```

`total_opens` is untouched by this — confirm with another
`cat /proc/procfs_demo/info` afterward: `event_count` resets to 0,
`total_opens` keeps its prior value and keeps climbing. The two counters
have independent lifetimes, exactly as their separate reset logic in the
source implies.

## Step 11 — clean up

**`break procfs_seqfile_exit` accepts with no error but never fires** —
it's marked `__exit`, which places it in the `.exit.text` ELF section,
and `lx-symbols` never relocates that section. The breakpoint resolves
to a tiny raw file offset instead of a real kernel address; `rmmod`
would complete underneath it while GDB just sits at `Continuing.`
forever. The fix: break on the generic kernel hook every `rmmod` goes
through instead.

**Pane: gdb**

```
info breakpoints
```
```
delete N
```

(The `events_write` breakpoint's number from step 10.)

```
break __do_sys_delete_module
```
```
continue
```

**Pane: vmb**

```bash
rmmod procfs_seqfile
```

**Pane: gdb**

```
Thread N hit Breakpoint N, __do_sys_delete_module (...) at kernel/module/main.c:808
```
```
advance kernel/module/main.c:863
```
```
863         mod->exit();
```
```
print mod->exit
```
```
$1 = (void (*)(void)) 0xffff80007c32b8b0
```

(That exact address is from one real run — yours will differ, module
memory placement is random per boot even with `nokaslr`. Use whatever
`print mod->exit` gives you next.)

```
add-symbol-file /home/adiopocere/Desktop/codes/linux-kernel-project/06_procfs_seqfile/procfs_seqfile.ko -s .exit.text 0xffff80007c32b8b0
```
```
y
```
```
break procfs_seqfile_exit
```
```
Breakpoint N at 0xb8: procfs_seqfile_exit. (2 locations)
```

Two locations now — `N.1` is the old broken raw-offset one, `N.2` is the
newly-relocated real one:

```
disable N.1
```
```
continue
```

**Pane: vmb**

```bash
rmmod procfs_seqfile
```

**Pane: gdb**

```
Thread N hit Breakpoint N.2, 0xffff80007c32b8b4 in cleanup_module ()
```

Real hit, real name — `module_exit()` aliases it to `cleanup_module`,
the same mechanism modules 01/02 cover for `init_module`.

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

The `seq_file` start/next/show/stop contract is not a metaphor — it's
four real, separately-breakable function calls the VFS makes, in a fixed
order, once per `read()` (not once per file lifetime), and the
`stop()`-always-runs guarantee is something you can force yourself to
watch happen on the specific iteration that returns `NULL`, rather than
trust from a comment (step 8). `total_opens` vs. `event_count` diverging
under two different reset lifetimes (steps 9–10) is the same lesson
`open_release_cdev` (module 08) teaches with a single pointer: read the
actual live state instead of trusting what the source comment claims it
should be. Every `/proc` or debugfs listing in the kernel that shows more
than one row works this exact way underneath.
