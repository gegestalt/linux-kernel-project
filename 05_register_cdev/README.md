# 05 — register_cdev

The first character device in this repo: register one, read from it, watch
the kernel-log accounting.

## What this demonstrates

- `register_chrdev(0, name, fops)` — the old, simple char-device API. Major
  `0` asks the kernel to allocate a free major number dynamically rather
  than claiming a fixed, potentially-already-taken one; the allocated major
  is the function's return value. (Lab [09](../09_read_write_cdev/) and
  onward move to the modern `alloc_chrdev_region()` + `cdev_init()` +
  `cdev_add()` API, which supports more than one minor cleanly — worth
  contrasting once you get there.)
- `struct file_operations` as the connection between VFS syscalls and this
  module's callbacks — `open`, `read`, `release` here.
- Reading through the kernel/user boundary correctly: the response string
  is built in a **kernel-stack** buffer and handed to userspace with
  `copy_to_user()`, never a raw pointer dereference.
- Respecting the `read()` contract: returning `0` at EOF (once `*offset`
  reaches the message length) so tools like `cat` know to stop, and
  clamping `bytes_to_copy` to whatever the caller actually asked for.
- `atomic_t` for a simple concurrent-safe counter (`open_count`) — the
  minimal member of the toolkit lab [11](../11_concurrency_locking/) covers
  in full (atomics vs spinlocks vs mutexes).
- `imajor()`/`iminor()` to recover device numbers from a `struct inode`,
  and `current->comm`/`current->pid` to log which process is on the other
  end of every callback.
- A `checkpatch.pl`-backed `make check` target — see below.

## Files

| File | Purpose |
|---|---|
| `register_cdev.c` | The driver. |
| `Makefile` | Build (`all`), `clean`, and `check`/`checkpatch` targets. |

## Build

```bash
cd 05_register_cdev
make
```

## Load, create the device node, and test

Unlike lab 03's `misc_register()`, `register_chrdev()` does **not** create
`/dev` entries or a sysfs class for you — you make the node yourself once
you know the major number the kernel handed back.

```bash
sudo insmod ./register_cdev.ko
dmesg | tail -5                       # note the allocated major number, e.g. "major=241"

MAJOR=$(dmesg | grep -oP 'register_cdev: init: major=\K[0-9]+' | tail -1)
sudo mknod /dev/register_cdev c "$MAJOR" 0
sudo chmod 666 /dev/register_cdev      # or read as root; your call

cat /dev/register_cdev
# register_cdev kernel device
# major=<MAJOR>
# minor=0
# context=cat[<pid>]

dmesg | tail -10   # one "open:", one "read:", one "release:" line per `cat`
```

Open it multiple times concurrently to watch `open_count` move:

```bash
exec 3</dev/register_cdev   # open and hold fd 3 open in this shell
dmesg | tail -3              # opens=1
cat /dev/register_cdev       # a second, independent open+read+release; opens briefly =2
exec 3<&-                    # close fd 3
dmesg | tail -3              # opens=0
```

Try reading from a different minor — `register_chrdev()` claims the whole
major (minors 0–255), and this driver ignores the minor entirely except to
report it:

```bash
sudo mknod /dev/register_cdev1 c "$MAJOR" 1
cat /dev/register_cdev1   # works identically, reports minor=1
```

## checkpatch

```bash
make check      # or: make checkpatch
```

Runs `scripts/checkpatch.pl --strict` from the vendored `../../linux_mainline`
tree against `register_cdev.c`. `--ignore=PREFER_PR_LEVEL` is set in the
Makefile only for this lab's sibling `07_log_level`; here it's a plain
strict run — try deliberately breaking a style rule (e.g. a line over 100
columns, or a `printk()` without a matching `pr_fmt()`-driven level) and
rerun `make check` to see checkpatch catch it.

## Cleanup

```bash
sudo rm -f /dev/register_cdev /dev/register_cdev1
sudo rmmod register_cdev
dmesg | tail -5    # exit log: opens=0 (assuming everything above was closed), uptime
make clean
```

## Things to try

- `insmod` this module twice without `rmmod`-ing first — the second load
  gets a *different* dynamic major, and both instances coexist
  independently (each with its own `open_count`) until each is separately
  removed. Convince yourself with `lsmod` and two different `mknod`s.
- Redirect a large amount of data at it — `dd if=/dev/zero of=/dev/register_cdev`
  — and read the error. This driver has no `write` callback at all, so the
  VFS returns `-EINVAL` before ever calling into this module; confirm with
  `strace dd if=/dev/zero of=/dev/register_cdev count=1`.
- `cat /proc/devices | grep register_cdev` while the module is loaded — the
  same major you extracted from `dmesg` should show up there, which is
  actually a more idiomatic way to discover it than grepping the kernel log
  (useful once you don't control the modinsert-time log yourself).

## Debugging with GDB

Setup: [`../GDB_DEBUGGING.md`](../GDB_DEBUGGING.md).

```gdb
(gdb) lx-symbols /home/adiopocere/Desktop/codes/linux-kernel-project
(gdb) break register_cdev_read
(gdb) continue
```
```bash
cat /dev/register_cdev
```
```gdb
(gdb) print *offset                # starts at 0 on a fresh open
(gdb) next                          # past the scnprintf() building `message`
(gdb) print message                 # the exact kernel-stack buffer about to cross to userspace
(gdb) print bytes_to_copy
(gdb) next                          # step over copy_to_user()
(gdb) print *offset                 # advanced by bytes_to_copy
```

Because `message` is a plain stack array (not heap-allocated), this is a
good lab for getting comfortable with `print message` on a local array
versus `print buffer` on a `kzalloc()`'d pointer elsewhere in this repo
(lab 09) — same debugger command, genuinely different kind of memory
underneath.

The rest of this lab's targets, all verified first:

```bash
$ gdb -q -batch -nx -ex "file register_cdev.ko" \
    -ex "info line register_cdev_open" -ex "info line register_cdev_release" \
    -ex "info line register_cdev_init" -ex "ptype open_count" register_cdev.ko
Line 26 of "register_cdev.c" starts at address 0x188 <register_cdev_open> ...
Line 97 of "register_cdev.c" starts at address 0xc8 <register_cdev_release> ...
Line 127 of "register_cdev.c" starts at address 0x8b0 <register_cdev_init> ...
type = struct {
    int counter;
}
```

**`register_cdev_open`/`register_cdev_release` — watch `open_count`
change under a real `atomic_t`, live**, exactly matching the
two-concurrent-opens exercise above:

```gdb
(gdb) lx-symbols /home/adiopocere/Desktop/codes/linux-kernel-project
(gdb) break register_cdev_open
(gdb) break register_cdev_release
(gdb) continue
```
```bash
exec 3</dev/register_cdev    # holds an open fd, does NOT close it yet
```
```gdb
(gdb) print open_count                # {counter = 1} - the raw atomic_t struct
(gdb) print open_count.counter         # 1 - the plain int inside it
(gdb) print imajor(inode)               # cross-check against the major you mknod'd with
(gdb) print iminor(inode)
(gdb) continue
```
```bash
exec 3<&-    # now close it
```
```gdb
# breaks in register_cdev_release this time
(gdb) print open_count.counter         # back to 0
(gdb) finish
```

`print open_count` on the raw `atomic_t` is worth doing once just to see
that it's genuinely a one-member struct wrapping a plain `int` — the
"atomic" part is entirely in *how* `atomic_inc_return()`/
`atomic_dec_return()` touch that `int` (a single hardware-guaranteed
instruction), not in the type itself looking any different from a
normal counter.

**`register_cdev_init` — the dynamic major allocation**:

```gdb
(gdb) break register_cdev_init
(gdb) continue
```
```bash
sudo insmod ./register_cdev.ko
```
```gdb
(gdb) next     # past register_chrdev(0, DEVICE_NAME, &register_cdev_fops)
(gdb) print major   # whatever the kernel picked - compare against dmesg's own report
(gdb) finish
```

**`register_cdev_exit`** resolves the same way lab 03's `gpioctrl_exit`
does — its entry line lands inside `timekeeping.h` because its first
real statement inlines `ktime_get_ns()`. Still a perfectly real,
breakable symbol (`break register_cdev_exit` works normally); see lab
03's README for the full explanation of why `info line` reports it that
way.

