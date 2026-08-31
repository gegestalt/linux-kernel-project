# 06 — procfs_seqfile

The `/proc` filesystem, in two shapes: a single-value file, and a proper
multi-record listing built on the `seq_file` iterator API — the same
machinery behind real files like `/proc/PID/maps` or `/proc/interrupts`.

## What this demonstrates

- `proc_mkdir()` / `proc_create()` / `proc_create_single()` / `proc_remove()`
  — the directory and file lifecycle for `/proc` entries, and why
  `proc_remove()` on the top directory is enough to tear down everything
  underneath it (no need to individually remove each file first).
- `struct proc_ops` (the modern replacement for wiring `struct
  file_operations` directly into `/proc` since Linux 5.6) — note it's a
  *different* struct with differently-named fields (`proc_open`,
  `proc_read`, ...) from the `file_operations` every other lab in this repo
  uses for `/dev` nodes.
- **`proc_create_single()`** for the simple case: `info_show()` is a bare
  seq_file `show()` callback with no start/next/stop machinery — the
  kernel handles the "there's exactly one record" iteration for you.
- **The full `seq_file` iterator contract** (`start`/`next`/`stop`/`show`)
  for the general case, where there's a list to walk rather than one value
  to print. This is the part worth sitting with: `start()` and `stop()` are
  called once per `read()` syscall (not once per file open), and `stop()`
  always runs to match `start()` — even if `start()` returned `NULL` — which
  is why the lock lives there rather than being taken/dropped per-element
  inside `show()`.
- A `/proc` file that's both readable *and* writable
  (`echo clear > .../events`), via `proc_ops.proc_write`, using the same
  `copy_from_user()` pattern as the character devices in labs 05/09.
- Self-logging: opening `/proc/procfs_demo/events` is itself recorded as an
  event, so the act of reading the log grows it — a deliberately visible
  side effect to make the iterator's data change under you between reads.

## Files

| File | Purpose |
|---|---|
| `procfs_seqfile.c` | The module: `/proc/procfs_demo/info` (single value) and `/proc/procfs_demo/events` (seq_file listing, read + write). |
| `Makefile` | Build, `clean`, `check`/`checkpatch`. |

## Build

```bash
cd 06_procfs_seqfile
make
```

## Load and test

```bash
sudo insmod ./procfs_seqfile.ko
dmesg | tail -3

cat /proc/procfs_demo/info
# uptime_ms=...
# total_opens=0
# events_recorded=0 (capacity 64)

cat /proc/procfs_demo/events    # first open: logs itself, prints one line
cat /proc/procfs_demo/events    # second open: now two lines
cat /proc/procfs_demo/info      # total_opens has moved
```

Watch the iterator serve data across multiple, larger reads (forces more
than one `start()`/`stop()` cycle per invocation) by generating enough
records to matter, then reading with a tiny buffer:

```bash
for i in $(seq 1 20); do cat /proc/procfs_demo/events > /dev/null; done
dd if=/proc/procfs_demo/events bs=32 count=1000 2>/dev/null | wc -l
```

Clear the log:

```bash
echo clear | sudo tee /proc/procfs_demo/events
cat /proc/procfs_demo/events    # back down to just this one open
```

Fill it past capacity and confirm the bounded-log behavior:

```bash
echo clear | sudo tee /proc/procfs_demo/events > /dev/null
for i in $(seq 1 70); do cat /proc/procfs_demo/events > /dev/null; done
cat /proc/procfs_demo/info
# events_recorded=64 (capacity 64) -- capped, even though total_opens > 64
```

Invalid writes are rejected distinctly from valid ones:

```bash
echo nonsense | sudo tee /proc/procfs_demo/events
# tee: /proc/procfs_demo/events: Invalid argument
```

## checkpatch

```bash
make check
```

## Cleanup

```bash
sudo rmmod procfs_seqfile
dmesg | tail -3
ls /proc/procfs_demo    # No such file or directory - proc_remove() tore down the whole subtree
make clean
```

## Things to try

- Compare `/proc/procfs_demo/info`'s implementation against
  `/proc/procfs_demo/events`'s. Both ultimately call `seq_printf()`; the
  difference is entirely in how much iterator scaffolding each needs.
  Delete `events_seq_next()`'s early `return NULL` and see checkpatch's/the
  compiler's reaction to an iterator that can never terminate (don't
  actually load that version — reason through why it would hang `cat`).
- Open `/proc/procfs_demo/events` with a program that reads one byte at a
  time (`dd bs=1`) while another shell writes `clear` to it mid-read.
  `events_lock` is held across the whole `start()`...`stop()` window for a
  single `read()` call, but *not* across separate `read()` calls — work out
  what a reader can observe (a torn record list, or just an early
  truncation) with that granularity.
- Read `fs/proc/generic.c` and `include/linux/seq_file.h` in
  `../../linux_mainline` and find `seq_read()`/`seq_lseek()` — the generic
  functions this module borrows directly instead of writing its own
  `read`/`llseek`. This is the same "supply data, borrow generic glue"
  shape as `simple_read_from_buffer()` in lab 03.

## Debugging with GDB

For a full, self-contained, step-by-step session for this lab — tmux
pane layout, every command, every output explained — see
[`gdb_walkthrough.md`](gdb_walkthrough.md).

Setup: [`../gdb_debugging.md`](../gdb_debugging.md). This lab is the
best one in the repo for actually *watching* the seq_file iterator
contract run, rather than just reading about the order it's called in:

```gdb
(gdb) lx-symbols /home/adiopocere/Desktop/codes/linux-kernel-project
(gdb) break events_seq_start
(gdb) break events_seq_next
(gdb) break events_seq_show
(gdb) break events_seq_stop
(gdb) continue
```
```bash
cat /proc/procfs_demo/events
```

Each `continue` from here lands on the next callback in the sequence —
you'll see `start` once, then `show`/`next` alternating once per
buffered event, then `stop` exactly once at the end, even if
`event_count` is 0 (confirm this by running `echo clear | sudo tee
/proc/procfs_demo/events` first, so `start` immediately returns `NULL`
and you can watch `stop` still fire to match it). `print *pos` in
`events_seq_next` is the clearest way to see the iterator's position
counter advancing one record at a time.

Two more targets, verified first:

```bash
$ gdb -q -batch -nx -ex "file procfs_seqfile.ko" \
    -ex "info line info_show" -ex "info line events_write" \
    -ex "info line record_event" -ex "ptype struct proc_event" procfs_seqfile.ko
Line 79 of "procfs_seqfile.c" starts at address 0x4e8 <info_show> ...
Line 171 of "procfs_seqfile.c" starts at address 0x5d0 <events_write> ...
Line 53 of "procfs_seqfile.c" starts at address 0x364 <events_open+28> ...   # inlined!
type = struct proc_event {
    u64 ns;
    pid_t pid;
    char comm[16];
}
```

`record_event` has no symbol of its own — GCC inlined it entirely into
`events_open` (the address lands at `events_open+28`, not a standalone
function). Break on `events_open` instead to watch it happen:

```gdb
(gdb) break events_open
(gdb) continue
```
```bash
cat /proc/procfs_demo/events
```
```gdb
(gdb) next        # into the inlined record_event() body - next walks straight through it
(gdb) print event_count           # about to become one higher
(gdb) next
(gdb) print events[event_count - 1]     # the proc_event struct this open() just recorded
(gdb) print events[event_count - 1].comm   # "cat" - confirmed straight from kernel memory
(gdb) finish
```

**`info_show` — the single-value file**, worth contrasting directly
against the four-callback machinery above: one breakpoint, one
`seq_printf()`, done.

```gdb
(gdb) break info_show
(gdb) continue
```
```bash
cat /proc/procfs_demo/info
```
```gdb
(gdb) print total_opens
(gdb) print event_count
(gdb) next
(gdb) finish
```

**`events_write` — the `clear` command handler**:

```gdb
(gdb) break events_write
(gdb) continue
```
```bash
echo clear | sudo tee /proc/procfs_demo/events
```
```gdb
(gdb) print count               # 6 - "clear\n"
(gdb) next                       # copy_from_user(), then sysfs_streq(strim(kbuf), "clear")
(gdb) print kbuf
(gdb) next
(gdb) print event_count          # about to be reset to 0
(gdb) finish
```

Send anything other than `clear` and re-break here — `print kbuf` shows
your actual text, and stepping past the `sysfs_streq()` check lands on
`return -EINVAL;` instead of the reset, matching the "invalid writes are
rejected" behavior from this lab's own "Load and test" section.

