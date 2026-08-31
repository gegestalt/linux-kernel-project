# GDB walkthrough — 09_read_write_cdev

`read_write_cdev.c` is the modern char-device registration sequence —
`alloc_chrdev_region()` + `cdev_init()`/`cdev_add()` +
`class_create()` + `device_create()` — four separate calls in place of
lab 05's single `register_chrdev()`, with the payoff that `udev`
creates `/dev/read_write_cdev0` automatically. The debugging focus is
the read/write buffer itself: a fixed 4096-byte in-kernel buffer with
two independently-tracked sizes (`data_len`, how much has actually been
written; `BUF_SIZE`, how much exists at all) and a real seekable
position — the first lab where `read()`/`write()` genuinely interact
with each other's effects on the same underlying storage.

## Environment

```bash
cd 09_read_write_cdev
make -C /home/adiopocere/Desktop/codes/linux_mainline M=$(pwd) modules
modinfo read_write_cdev.ko | grep vermagic
sudo mount -o loop /home/adiopocere/Desktop/codes/qemu-vmb/labs-disk.img /tmp/vmb-mnt
sudo mkdir -p /tmp/vmb-mnt/09_read_write_cdev
sudo cp read_write_cdev.ko /tmp/vmb-mnt/09_read_write_cdev/
sudo umount /tmp/vmb-mnt
```

## tmux layout

Standard `vmb` + `gdbsess` — [`../GDB_DEBUGGING.md`](../GDB_DEBUGGING.md).

## The walkthrough

### Step 1 — init: watch the four-step registration sequence itself

```gdb
(gdb) target remote :1234
(gdb) lx-version
(gdb) break do_init_module
(gdb) continue
```
```bash
# vmb:
insmod /mnt/labs/09_read_write_cdev/read_write_cdev.ko
```
```gdb
(gdb) lx-symbols /home/adiopocere/Desktop/codes/linux-kernel-project
(gdb) break read_write_cdev_init
(gdb) continue
Thread 2 hit Breakpoint N, read_write_cdev_init () at read_write_cdev.c:166
(gdb) next   # kzalloc(BUF_SIZE, GFP_KERNEL)
(gdb) print buffer
$1 = (char *) 0xffff... ""
(gdb) next    # alloc_chrdev_region(&devt, 0, 1, DEVICE_NAME)
(gdb) print devt
(gdb) print MAJOR(devt)
```
Actually calling `MAJOR()` as a live function/macro invocation may not
resolve on a KGDB target the way it does in userspace GDB — if `print
MAJOR(devt)` errors, decode it by hand instead (it's `devt >> 20` on
this kernel's `dev_t` layout):
```gdb
(gdb) print devt >> 20
(gdb) next   # cdev_init(&rw_cdev, &rw_fops) + cdev_add()
(gdb) print rw_cdev.ops
$2 = (const struct file_operations *) 0x... <rw_fops>
```

`rw_cdev.ops` pointing directly at the file-scope `rw_fops` symbol (not
just some address — GDB resolves it back to the actual symbol name) is
the concrete proof that `cdev_init(&rw_cdev, &rw_fops)` did what it
claims: bound this exact function table to this exact device.

```gdb
(gdb) next   # class_create
(gdb) print rw_class
(gdb) next    # device_create - the call that actually makes udev create /dev/read_write_cdev0
(gdb) finish
```
```bash
# vmb:
ls -la /dev/read_write_cdev0
```

### Step 2 — write: watch `data_len` grow, distinct from `BUF_SIZE`

```gdb
(gdb) break rw_write
(gdb) continue
```
```bash
# vmb:
printf 'hello gdb' > /dev/read_write_cdev0
```
```gdb
Thread 2 hit Breakpoint N, rw_write (...) at read_write_cdev.c:107
(gdb) print count
$3 = 9
(gdb) print data_len
$4 = 0          # nothing written yet on this load
(gdb) next   # mutex_lock
(gdb) next    # the *ppos bounds check
(gdb) next     # space = BUF_SIZE - *ppos
(gdb) print space
$5 = 4096
(gdb) next   # to_copy = min(count, space)
(gdb) print to_copy
$6 = 9
(gdb) next    # copy_from_user
(gdb) next     # *ppos += to_copy
(gdb) next      # the data_len update
(gdb) print data_len
$7 = 9
```

### Step 3 — the short-write case: prove the buffer's ceiling is real

The source comment is explicit that a short write only ever happens
when the buffer is genuinely full. Force it directly rather than trust
the comment — and note this driver does **not** implement `O_APPEND`
semantics itself (it never calls `generic_write_checks()`/inspects
`file->f_flags & O_APPEND`), so a fresh `open()` always starts `*ppos`
at 0 regardless of how the fd was opened. To actually push `*ppos` up
to `BUF_SIZE`, hold **one** fd open across two writes with the shell's
own `exec`, rather than two separate redirections (which would each
start over at position 0):

```bash
# vmb:
exec 3> /dev/read_write_cdev0
dd if=/dev/zero bs=1 count=4096 2>/dev/null | tr '\0' 'x' >&3
```
```gdb
(gdb) break rw_write
(gdb) continue
Thread 2 hit Breakpoint N, rw_write (...) at read_write_cdev.c:107
(gdb) print count
$8 = 4096
(gdb) continue
```
```bash
# vmb, same fd 3, position now sits at BUF_SIZE from that last write:
printf 'overflow' >&3
```
```gdb
Thread 2 hit Breakpoint N, rw_write (...) at read_write_cdev.c:107
(gdb) print *ppos
$9 = 4096
(gdb) next    # *ppos < 0 || (size_t)*ppos >= BUF_SIZE  - now true
(gdb) finish
Value returned is $10 = -28    # -ENOSPC
```

`-28` is `-ENOSPC` (`errno.h`'s value 28 for `ENOSPC`, negated per
kernel convention) — GDB shows you the literal integer the function
returned; matching it to the symbolic name is on you, the same way it
would be reading a raw strace. This is a case `write_write_cdev.c`
never reaches through `rw_write`'s own `to_copy < count` short-write
path in this exact scenario (a write starting exactly at capacity hits
the earlier `*ppos >= BUF_SIZE` check instead) — worth noticing the two
different code paths that can each produce "your write didn't fully
succeed," with different concrete `errno`s.

### Step 4 — `llseek`: a real, boundary-checked reposition

```gdb
(gdb) delete
(gdb) break rw_llseek
(gdb) continue
```
```bash
# vmb:
dd if=/dev/read_write_cdev0 bs=1 skip=4090 count=6 2>/dev/null
```
```gdb
Thread 2 hit Breakpoint N, rw_llseek (file=0x..., offset=4090, whence=0) at read_write_cdev.c:152
(gdb) step   # into fixed_size_llseek() itself
```

`fixed_size_llseek()` is generic kernel code (`fs/read_write.c`),
shared by every driver with this exact "fixed-capacity, randomly
addressable" shape — stepping into it here is a quick way to see the
`SEEK_SET`/`SEEK_CUR`/`SEEK_END`/bounds-checking logic your own driver
gets for free by calling it, rather than re-implementing it.

## Cleanup

```gdb
(gdb) delete
(gdb) break read_write_cdev_exit
(gdb) continue
```
```bash
# vmb:
rmmod read_write_cdev
```
```gdb
(gdb) print data_len
```
```bash
# vmb:
poweroff -f
```

## What this proves

`data_len` and `BUF_SIZE` are genuinely independent quantities tracked
by different mechanisms — one a compile-time constant, one runtime
state updated on every successful write — and stepping through both
the "fits fine" and "buffer is full" branches with real writes shows
exactly which condition trips which return path, down to the specific
negative `errno` value the function actually returns. `rw_cdev.ops`
resolving back to the symbol `rw_fops` is the same kind of direct proof
lab 08 got from `filp->private_data`: GDB reading a live pointer and
recognizing what it points to beats reading the `cdev_init()` call and
assuming it worked.
