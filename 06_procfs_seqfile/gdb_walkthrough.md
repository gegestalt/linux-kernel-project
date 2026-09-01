# GDB walkthrough — 06_procfs_seqfile

`procfs_seqfile.c` implements the full `seq_file` iterator contract —
the same `start()`/`next()`/`show()`/`stop()` protocol the kernel uses
internally for things like `/proc/PID/maps`. The driver's own comment
block explains the calling convention; this walkthrough's job is to
make it *observable*: breaking on all four callbacks at once and
watching the exact call sequence a single `cat /proc/procfs_demo/events`
actually produces, including the one detail that's easy to miss reading
the source alone — `stop()` runs even on the call where `start()`
returned `NULL`.

## Environment

```bash
cd 06_procfs_seqfile
make -C /home/adiopocere/Desktop/codes/linux_mainline M=$(pwd) modules
modinfo procfs_seqfile.ko | grep vermagic
sudo mount -o loop /home/adiopocere/Desktop/codes/qemu-vmb/labs-disk.img /tmp/vmb-mnt
sudo mkdir -p /tmp/vmb-mnt/06_procfs_seqfile
sudo cp procfs_seqfile.ko /tmp/vmb-mnt/06_procfs_seqfile/
sudo umount /tmp/vmb-mnt
```

## tmux layout

Standard `vmb` + `gdbsess` — [`../gdb_debugging.md`](../gdb_debugging.md).

## Real, verified breakpoint targets

```
Line 120: events_seq_start
Line 130: events_seq_next
Line 145: events_seq_show
Line 140: events_seq_stop
Line 79:  info_show           (the single-value /proc/procfs_demo/info)
```

`record_event()` (called from `events_open()`) is small enough to be
inlined into its caller — `info line record_event` resolves to
`events_open+28`, not a separate function, confirmed statically; break
on `events_open` itself if you want to catch the moment an `open()`
of `events` logs itself.

### Step 0 — get some events into the log first

`events_seq_start`/`next` walk a real, non-empty array only if
something has opened `/proc/procfs_demo/events` before now — each open
is itself what `record_event()` logs. Load the module and generate a
few entries before setting any seq_file breakpoints, so step 1 has
actual multi-element iteration to show, not a single-row edge case:

```gdb
(gdb) target remote :1234
(gdb) lx-version
(gdb) break do_init_module
(gdb) continue
```
```bash
# vmb:
insmod /mnt/labs/06_procfs_seqfile/procfs_seqfile.ko
cat /proc/procfs_demo/events > /dev/null
cat /proc/procfs_demo/events > /dev/null
cat /proc/procfs_demo/events > /dev/null
```
```gdb
(gdb) lx-symbols /home/adiopocere/Desktop/codes/linux-kernel-project
```

### Step 1 — the four-callback sequence, live, for one `cat`

```gdb
(gdb) break events_seq_start
(gdb) break events_seq_next
(gdb) break events_seq_show
(gdb) break events_seq_stop
(gdb) continue
```
```bash
# vmb:
cat /proc/procfs_demo/events
```

Now just keep `continue`-ing and read off which breakpoint fires each
time — this is the entire lesson, made visible one hit at a time:

```gdb
Thread 2 hit Breakpoint 1, events_seq_start (s=0x..., pos=0x...) at procfs_seqfile.c:120
(gdb) print *pos
$1 = 0
(gdb) continue
Thread 2 hit Breakpoint 3, events_seq_show (s=0x..., v=0x...) at procfs_seqfile.c:145
(gdb) print ((struct proc_event *)v)->pid
(gdb) continue
Thread 2 hit Breakpoint 2, events_seq_next (s=0x..., v=0x..., pos=0x...) at procfs_seqfile.c:130
(gdb) print *pos
$2 = 0          # not yet incremented - next's own (*pos)++ hasn't executed
(gdb) next
(gdb) print *pos
$3 = 1
(gdb) continue
```

Repeat `continue` through the `show`/`next` pairs — one per logged
event (3, from step 0's three `cat`s, plus this one's own open makes
4). On the very last iteration, `events_seq_next` will set `*pos` past
`event_count` and return `NULL`, which is what finally ends the loop
— catch it directly:

```gdb
Thread 2 hit Breakpoint 2, events_seq_next (...) at procfs_seqfile.c:130
(gdb) next
(gdb) print *pos
(gdb) print event_count
# once *pos >= event_count:
(gdb) finish
Value returned is $4 = (void *) 0x0
```

And finally:

```gdb
Thread 2 hit Breakpoint 4, events_seq_stop (s=0x..., v=0x...) at procfs_seqfile.c:140
(gdb) print v
$5 = (void *) 0x0
```

**This is the detail worth stopping on.** `events_seq_stop` just fired
with `v == NULL` — the exact value `events_seq_next` returned to signal
"no more elements." The source comment in `procfs_seqfile.c` states
this ("stop() is always called to match start(), even on error paths
and even when start() itself returned NULL") as a documentation claim;
you've now watched it actually happen, on the iteration where there was
nothing left to show. This is precisely why `events_lock` gets taken in
`start()` and dropped in `stop()` rather than per-element in `show()` —
`stop()` is the one callback in this protocol guaranteed to run
regardless of how the iteration ended, so it's the only safe place to
release something `start()` acquired unconditionally.

### Step 2 — `info_show`: the other, simpler proc file

`proc_create_single()` skips the whole start/next/stop dance for a
file with exactly one thing to print:

```gdb
(gdb) info breakpoints
(gdb) delete <the four events_seq_* breakpoint numbers from step 1>
(gdb) break info_show
(gdb) continue
```

(Bare `delete` with no numbers deletes *everything*, but first asks
`Delete all breakpoints? (y or n)` — if that prompt's confirmation gets
swallowed by a command typed right after it, nothing actually gets
deleted and you're left debugging against stale breakpoints. Naming
numbers explicitly, checked with `info breakpoints` first, skips the
prompt entirely.)
```bash
# vmb:
cat /proc/procfs_demo/info
```
```gdb
Thread 2 hit Breakpoint N, info_show (s=0x..., v=0x...) at procfs_seqfile.c:79
(gdb) next   # ktime_get_ns() - loaded_at_ns
(gdb) print uptime_ms
(gdb) next    # seq_printf total_opens
(gdb) print total_opens
```

Compare `total_opens` here against `event_count` from step 1 —
`total_opens` incremented on *every* open of `events` (step 0's three
plus step 1's one, so 4), while `event_count` only grows up to
`MAX_EVENTS` (64) before the array itself stops accepting new records
even though `total_opens` keeps counting — the exact bounded-log
behavior the source comment describes, now backed by two numbers you
read directly out of memory rather than took on faith.

### Step 3 — the write path: clearing the log

```gdb
(gdb) info breakpoints
(gdb) delete <the info_show breakpoint's number from step 2>
(gdb) break events_write
(gdb) continue
```
```bash
# vmb:
echo clear > /proc/procfs_demo/events
```
```gdb
Thread 2 hit Breakpoint N, events_write (...) at procfs_seqfile.c:169
(gdb) next    # past copy_from_user
(gdb) print kbuf
(gdb) next     # sysfs_streq(strim(kbuf), "clear") check
(gdb) next      # event_count = 0
(gdb) print event_count
$6 = 0
```

`total_opens` is untouched by this — confirm with another
`cat /proc/procfs_demo/info` afterward: `event_count` resets to 0,
`total_opens` keeps its prior value and continues climbing. The two
counters really do have independent lifetimes, exactly as their
separate reset logic in the source implies.

## Cleanup

**`break procfs_seqfile_exit` accepts with no error but never fires** —
confirmed live. It's marked `__exit`, which places it in the
`.exit.text` ELF section, and `lx-symbols` never relocates that section
(full diagnosis, straight from this kernel's own
`scripts/gdb/linux/symbols.py`, in module 02's and 12's walkthroughs).
The breakpoint resolves to a tiny raw file offset instead of a real
kernel address; `rmmod` completes underneath it while GDB just sits at
`Continuing.` forever.

**The working fix**:

```gdb
(gdb) info breakpoints
(gdb) delete <the events_write breakpoint's number from step 3>
(gdb) break __do_sys_delete_module
(gdb) continue
```
```bash
# vmb:
rmmod procfs_seqfile
```
```gdb
Thread N hit Breakpoint N, __do_sys_delete_module (...) at kernel/module/main.c:808
(gdb) advance kernel/module/main.c:863
863         mod->exit();
(gdb) print mod->exit
$1 = (void (*)(void)) 0xffff80007c32b8b0
(gdb) add-symbol-file /home/adiopocere/Desktop/codes/linux-kernel-project/06_procfs_seqfile/procfs_seqfile.ko -s .exit.text 0xffff80007c32b8b0
(y or n) y
(gdb) break procfs_seqfile_exit
Breakpoint N at 0xb8: procfs_seqfile_exit. (2 locations)
```

(The address is from one real run — use whatever `print mod->exit`
gives you; module memory placement is random per boot even with
`nokaslr`.) Disable the broken location, keep the relocated one:

```gdb
(gdb) disable N.1
(gdb) continue
```
```bash
# vmb:
rmmod procfs_seqfile
```
```gdb
Thread N hit Breakpoint N.2, 0xffff80007c32b8b4 in cleanup_module ()
```

Real hit, real name (`module_exit()` aliases it to `cleanup_module`,
same mechanism module 01/02 cover for `init_module`). `next`/`bt`/
`finish` work normally from here on any real vmlinux function it calls
into, even though `cleanup_module` itself has no line-by-line
resolution.

```bash
# vmb:
poweroff -f
```

## What this proves

The `seq_file` start/next/show/stop contract is not a metaphor — it's
four real, separately-breakable function calls the VFS makes, in a
fixed order, once per `read()` (not once per file lifetime), and the
`stop()`-always-runs guarantee is something you can force yourself to
watch happen on the specific iteration that returns `NULL`, rather than
trust from a comment. Every `/proc` or debugfs listing in the kernel
that shows more than one row works this exact way underneath.
