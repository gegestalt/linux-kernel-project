# 09 — read_write_cdev

A real read/write character device, backed by the *modern* char-device
registration sequence — the one that gives you an automatic `/dev` node
instead of a major number you `mknod` by hand.

## What this demonstrates

- The modern four-step device setup, in place of labs 05/08's
  `register_chrdev()`:
  1. `alloc_chrdev_region()` — reserve a `(major, minor..minor+count)` range.
  2. `cdev_init()` + `cdev_add()` — bind a `struct cdev` (and its
     `file_operations`) to that range.
  3. `class_create()` — register a device class in sysfs.
  4. `device_create()` — actually create the `/dev` node under that class.

  Step 4 is the payoff: `/dev/read_write_cdev0` exists the instant
  `insmod` returns, made by udev reacting to the class/device registration
  — no manual `mknod`, no reading a major number out of `dmesg` first.
- A genuine read/write path: `copy_to_user()` in `read()`,
  `copy_from_user()` in `write()`, both bounded correctly against the
  buffer's actual capacity (`BUF_SIZE`) and its currently-valid length
  (`data_len`) — two different bounds, tracked separately, and it matters
  which one each callback checks.
- Short reads/writes handled per the POSIX contract: `read()` returns `0`
  at true EOF (`*ppos >= data_len`, not `*ppos >= BUF_SIZE`); `write()`
  returns fewer bytes than requested — never an error — when the buffer
  fills up, and only returns `-ENOSPC` when *no* bytes at all can be
  written (`*ppos` already at `BUF_SIZE`).
- `fixed_size_llseek()` — a generic helper (not something this driver
  hand-rolls) that implements `SEEK_SET`/`SEEK_CUR`/`SEEK_END` correctly
  against a known fixed capacity, the same "borrow generic glue" pattern as
  lab [06](../06_procfs_seqfile/)'s `seq_read()`/`seq_lseek()`.
- `kzalloc(BUF_SIZE, GFP_KERNEL)` for the backing store, freed on every
  error path in `init()` and again in `exit()` — see lab
  [13](../13_kernel_memory/) for allocator choices in more depth.

## Files

| File | Purpose |
|---|---|
| `read_write_cdev.c` | The driver. |
| `Makefile` | Build, `clean`, `check`/`checkpatch`. |

## Build

```bash
cd 09_read_write_cdev
make
```

## Load and test

```bash
sudo insmod ./read_write_cdev.ko
dmesg | tail -3
ls -l /dev/read_write_cdev0     # created automatically — no mknod needed
```

Basic round-trip:

```bash
echo -n "hello kernel" | sudo tee /dev/read_write_cdev0 > /dev/null
sudo cat /dev/read_write_cdev0
# hello kernel
dmesg | tail -6                  # one write: line, one read: line, with byte counts
```

Seeking:

```bash
printf 'ABCDEFGHIJ' | sudo tee /dev/read_write_cdev0 > /dev/null
sudo dd if=/dev/read_write_cdev0 bs=1 skip=3 count=4 2>/dev/null; echo
# DEFG
```

Fill it past capacity and watch the short write:

```bash
sudo python3 - <<'EOF'
fd = open("/dev/read_write_cdev0", "wb")
data = b"x" * 5000        # bigger than BUF_SIZE=4096
written = fd.write(data)
print("wrote:", written)   # 4096, not 5000 — a legal short write
fd.close()
EOF
dmesg | tail -3
```

Then confirm a write that starts *already* at the end gets `-ENOSPC`
outright:

```bash
sudo python3 - <<'EOF'
fd = open("/dev/read_write_cdev0", "r+b")
fd.seek(4096)
try:
    fd.write(b"x")
except OSError as e:
    print("write failed as expected:", e)
EOF
```

## checkpatch

```bash
make check
```

## Cleanup

```bash
sudo rmmod read_write_cdev
dmesg | tail -3
ls /dev/read_write_cdev0    # gone — device_destroy() removed it
make clean
```

## Things to try

- Write less than `BUF_SIZE`, then read it back with `cat` *twice* in a
  row without reopening the fd in between (e.g. two separate `cat`
  invocations, which each `open()`+`read()`+`close()`) — confirm each
  fresh `open()` starts `*ppos` at `0` again, so both reads return the
  full content, unlike a single long-lived fd where a second `read()`
  would return `0` (EOF) immediately.
- Compare this driver's `read()`/`write()` bound-checking against lab
  [05](../05_register_cdev/)'s, which has no `write()` at all, and lab
  [03](../03_gpio_sim/)'s `gpioctrl_write()`, which parses a small command
  language instead of storing raw bytes. Three different answers to "what
  should `write()` even mean for this device," each appropriate to what
  the device represents.
- Deliberately swap the `data_len`/`BUF_SIZE` bound checked in `rw_read()`
  and `rw_write()` (read against `BUF_SIZE`, write against `data_len`).
  Rebuild, and work out — before testing it — what you'd now be able to
  read that was never written, and why capping writes at `data_len`
  instead of `BUF_SIZE` would make the buffer impossible to grow.

## Debugging with GDB

Setup: [`../GDB_DEBUGGING.md`](../GDB_DEBUGGING.md).

```gdb
(gdb) lx-symbols /home/adiopocere/Desktop/codes/linux-kernel-project
(gdb) break rw_write
(gdb) continue
```
```bash
echo -n "hello" | sudo tee /dev/read_write_cdev0
```
```gdb
(gdb) print count
(gdb) print *ppos
(gdb) print *buf@count            # read the __user source buffer directly - you're in tee's own mm context
(gdb) print $lx_current()->comm   # confirm it's really "tee"
(gdb) watch data_len               # continue; stops the instant data_len is updated
(gdb) next                          # walk copy_from_user(), the *ppos update, the clamp logic, line by line
```

To see the short-write path specifically, break here, then from the
guest push more than `BUF_SIZE` bytes at once (the big Python write in
this lab's "Load and test" section) and `print to_copy` vs `print
count` at the point `to_copy` gets clamped by `space` — the exact moment
this driver decides to return less than what was asked for.

