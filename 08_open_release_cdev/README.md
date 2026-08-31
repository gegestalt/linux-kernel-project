# 08 — open_release_cdev

What actually happens on `open()`/`close()`, and why "one `open()` call" and
"one `release()` call" aren't the same guarantee.

## What this demonstrates

- Per-open state stored in `filp->private_data`, allocated fresh in `open()`
  and freed in `release()` — the same struct pointer flows through every
  `read`/`write`/`ioctl` call for that particular open file description
  (this driver doesn't implement those, but the pattern here is exactly
  what labs [09](../09_read_write_cdev/) and [10](../10_ioctl_basics/) build
  on).
- The distinction between a **file descriptor** and an **open file
  description**: `dup()` creates a second fd referring to the *same* open
  file description (same `struct file`, same `private_data`). Closing one
  of the two duplicated fds does **not** trigger `release()` — only
  dropping the last reference does. `test.c`'s `run_dup_test()` makes this
  directly visible: one `OPEN`, then a delayed single `RELEASE` after
  *both* descriptors are closed.
- Decoding `filp->f_flags`/`filp->f_mode` — access mode
  (`O_RDONLY`/`O_WRONLY`/`O_RDWR`) plus flags like `O_NONBLOCK`,
  `O_APPEND`, `O_SYNC`, `O_DIRECT` — all set by the calling process at
  `open()` time and visible to the driver for the lifetime of that open.
- `atomic_t`/`atomic64_t` bookkeeping (`active_opens`, a monotonic
  `next_open_id`) so concurrent opens from multiple processes get distinct,
  traceable IDs in the log.
- Measuring an open's lifetime with `ktime_get_ns()` captured at `open()`
  and diffed at `release()`.

## Files

| File | Purpose |
|---|---|
| `open_release_cdev.c` | The driver: `my_open()`/`my_release()`, no read/write. |
| `test.c` | A userspace helper exercising four open-flag combinations plus the `dup()` case. |
| `Makefile` | Builds both the module (`module`) and `cdev_test` (from `test.c`); `check`/`checkpatch` lints both the kernel and userspace source. |

## Build

```bash
cd 08_open_release_cdev
make            # builds open_release_cdev.ko and cdev_test
```

## Load, create the device node, and test

```bash
sudo insmod ./open_release_cdev.ko
dmesg | tail -5
MAJOR=$(dmesg | grep -oP "registered 'open_release_cdev' major=\K[0-9]+" | tail -1)
sudo mknod /dev/open_release_cdev0 c "$MAJOR" 0
sudo chmod 666 /dev/open_release_cdev0

./cdev_test /dev/open_release_cdev0
```

Watch `dmesg -w` in another shell while `cdev_test` runs. You should see,
in order: four `OPEN #n` / `RELEASE #n` pairs (one per flag combination,
each immediately closed), then for the `dup()` test: a single `OPEN #5`,
then — only after `cdev_test` closes *both* the original and the
duplicated descriptor (there's a deliberate 1-second gap between them in
`test.c`) — a single `RELEASE #5`. Closing the first of the two produces no
`RELEASE` line at all.

Try it by hand to feel the fd/open-file-description distinction directly:

```bash
exec 3</dev/open_release_cdev0
exec 4<&3                 # fd 4 now duplicates fd 3 — same open file description
dmesg | tail -3            # one OPEN, active_opens=1
exec 3<&-                  # close one of the two
dmesg | tail -3            # nothing new — still open via fd 4
exec 4<&-                  # close the last reference
dmesg | tail -3            # now the RELEASE line appears, active_opens=0
```

## checkpatch (kernel + userspace)

```bash
make check       # or: make checkpatch
```

Runs `checkpatch.pl --strict` against `open_release_cdev.c`, then compiles
`test.c` with `-Wall -Wextra -Wpedantic -Werror -fsyntax-only` as a
userspace lint pass (`userspace-check`).

## Cleanup

```bash
sudo rm -f /dev/open_release_cdev0
sudo rmmod open_release_cdev
dmesg | tail -5    # exit log: active_opens should read 0, total_open_calls=5 after one full test.c run
make clean         # also removes the cdev_test binary
```

## Things to try

- Add your own `run_open_test()` call in `test.c` using `O_RDONLY |
  O_APPEND` — `O_APPEND` only really matters for `write()`, which this
  driver doesn't implement, so confirm the flag still shows up decoded in
  `dmesg` even though it has no behavioral effect here.
- Open the device from two different terminals at once and hold both open
  (`exec 3<...` in each) — confirm `active_opens` reaches 2 and each open
  gets its own monotonically increasing `id`, independent of which process
  opened it.
- Kill (`kill -9`) a shell that's holding the device open via `exec 3<...`
  instead of closing it cleanly — confirm `release()` still runs (the
  kernel drops the reference on process exit regardless of how it exits)
  by checking `dmesg` and `active_opens` afterward.

## Debugging with GDB

Setup: [`../GDB_DEBUGGING.md`](../GDB_DEBUGGING.md). This lab's whole
point is the `dup()`/reference-counting behavior, which is exactly the
kind of thing worth pausing mid-execution to actually see rather than
inferring from log lines:

```gdb
(gdb) lx-symbols /home/adiopocere/Desktop/codes/linux-kernel-project
(gdb) break my_open
(gdb) break my_release
(gdb) continue
```
```bash
./cdev_test /dev/open_release_cdev0
```

At the `my_open` breakpoint: `next` past the `kzalloc()`, `print ctx`
(your freshly allocated `struct open_context`), `next` again and
`print active_opens` before vs. after the `atomic_inc_return()` line.
For the `dup()` test specifically, stop at `my_release` and `print
ctx->id` — confirm you only ever break here **once** for the pair of
descriptors `test.c` duplicates, no matter how many times you'd expect
"a close" to trigger it. `bt` at that breakpoint also shows the real VFS
call chain (`__fput` → `...→ my_release`) rather than a raw `close()`.

`ptype struct open_context` (verified: `id`, `opened_ns`, `minor`,
`flags`, `mode`, `opener_pid`, `opener_comm[16]`) is worth running once
so `print *ctx` at any `my_open`/`my_release` breakpoint means something
— every field in it is something this driver captured *at open time*
and is still reading back at release, which is the entire mechanism
`filp->private_data` exists for.

**`open_release_cdev_init`/`open_release_cdev_exit`**, both verified:

```bash
$ gdb -q -batch -nx -ex "file open_release_cdev.ko" \
    -ex "info line open_release_cdev_init" -ex "info line open_release_cdev_exit" \
    open_release_cdev.ko
Line 172 of "open_release_cdev.c" starts at address 0xb90 <open_release_cdev_init> ...
Line 189 of "open_release_cdev.c" starts at address 0xc50 <open_release_cdev_exit> ...
```

```gdb
(gdb) break do_init_module
(gdb) continue
```
```bash
sudo insmod ./open_release_cdev.ko
```
```gdb
(gdb) lx-symbols /home/adiopocere/Desktop/codes/linux-kernel-project
(gdb) break open_release_cdev_init
(gdb) continue
(gdb) next        # register_chrdev(0, DEVICE_NAME, &fops)
(gdb) print major
(gdb) finish
```

At `open_release_cdev_exit` (break on it directly — it already exists
once the module is loaded, no catch-all needed), `print active_opens`
and `print next_open_id` before `unregister_chrdev()` runs — this is
the exact pair of numbers this lab's own `dmesg` exit log reports
(`active_opens=... total_open_calls=...`), now readable straight out of
the `atomic_t`/`atomic64_t` globals instead of parsing a log line.

