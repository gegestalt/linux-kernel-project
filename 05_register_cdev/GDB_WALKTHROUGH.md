# GDB walkthrough — 05_register_cdev

`register_cdev.c` is the legacy, one-call char device registration path
— `register_chrdev()` allocates a major dynamically and wires up
`file_operations` in a single step, with no `struct cdev`, no device
class, and (unlike lab 09's modern equivalent) no automatic `/dev` node
— you `mknod` it yourself once you've read the major out of `dmesg`.
The debugging focus here is the **open/read/release triangle**: three
separate callbacks, invoked by three separate syscalls, that all need
to agree on `struct inode`/`struct file` identity — and an `atomic_t`
open-count that's the simplest possible shared-state example before
lab 11 covers real races.

## Environment

```bash
cd 05_register_cdev
make -C /home/adiopocere/Desktop/codes/linux_mainline M=$(pwd) modules
modinfo register_cdev.ko | grep vermagic
sudo mount -o loop /home/adiopocere/Desktop/codes/qemu-vmb/labs-disk.img /tmp/vmb-mnt
sudo mkdir -p /tmp/vmb-mnt/05_register_cdev
sudo cp register_cdev.ko /tmp/vmb-mnt/05_register_cdev/
sudo umount /tmp/vmb-mnt
```

## tmux layout

Standard `vmb` + `gdbsess` pair — [`../GDB_DEBUGGING.md`](../GDB_DEBUGGING.md).

## The walkthrough

### Step 1 — init: watch the dynamic major get allocated

```gdb
(gdb) target remote :1234
(gdb) lx-version
(gdb) break do_init_module
(gdb) continue
```
```bash
# vmb:
insmod /mnt/labs/05_register_cdev/register_cdev.ko
```
```gdb
(gdb) lx-symbols /home/adiopocere/Desktop/codes/linux-kernel-project
(gdb) break register_cdev_init
```

Verified: `Line 127 of "register_cdev.c" starts at address 0x8b0
<register_cdev_init>`.

```gdb
(gdb) continue
Thread 2 hit Breakpoint N, register_cdev_init () at register_cdev.c:127
(gdb) next    # register_chrdev(0, DEVICE_NAME, &register_cdev_fops)
(gdb) print major
$1 = 240
```

The `0` passed as the first argument to `register_chrdev()` is a
request for a *dynamically* assigned major — `major` is whatever the
kernel actually handed back, which can differ run to run depending on
what else has claimed a major number. This is exactly why the driver
prints it (`pr_info("init: major=%d ...")`) rather than hardcoding
it anywhere: there is no other way for userspace to know it. You now
know it before `dmesg` even shows the line, straight from the
variable.

```gdb
(gdb) finish
```
```bash
# vmb:
dmesg | tail -2
mknod /dev/register_cdev0 c $(dmesg | grep -o 'major=[0-9]*' | tail -1 | cut -d= -f2) 0
```

### Step 2 — `open()`: `struct inode`/`struct file` as GDB sees them

```gdb
(gdb) break register_cdev_open
(gdb) continue
```
```bash
# vmb:
cat /dev/register_cdev0 &
```
```gdb
Thread 2 hit Breakpoint N, register_cdev_open (inode=0x..., file=0x...) at register_cdev.c:26
(gdb) print *inode
$2 = {i_mode = ..., i_rdev = ..., ...}
(gdb) print imajor(inode)
(gdb) print iminor(inode)
```

`imajor()`/`iminor()` are ordinary inline functions, not macros, so
GDB can call... actually, per KGDB's own limits (see
[`../GDB_DEBUGGING.md`](../GDB_DEBUGGING.md#a-note-on-function-calls-from-gdb)),
**don't** try `print imajor(inode)` as an actual function call on a
live KGDB target — instead read the same field it reads directly:

```gdb
(gdb) print inode->i_rdev
$3 = 61440    # MAJOR(i_rdev)<<20 | MINOR(i_rdev) - decode by hand, or just trust dmesg's own print
(gdb) print current->comm
$4 = "cat\000\000\000\000\000\000\000\000\000\000\000\000"
(gdb) print current->pid
```

`current` here really is `cat`'s own `task_struct` — `open()` runs in
the calling process's own context, synchronously, which is why
`current` is meaningful at all inside a syscall-driven callback (contrast
with lab 03's workqueue callback, where `current` was a `kworker`
instead of anything the user typed).

```gdb
(gdb) next   # past atomic_inc_return(&open_count)
(gdb) print count
```

### Step 3 — two opens at once: the atomic counter earns its keep

Background a second reader before releasing the first, so `open_count`
genuinely reaches 2 rather than bouncing straight back to 0/1:

```gdb
(gdb) continue
```
```bash
# vmb, while the first `cat` is still backgrounded (or use `sleep 5 < /dev/register_cdev0 &` to hold it open longer):
cat /dev/register_cdev0 &
```
```gdb
Thread 2 hit Breakpoint N, register_cdev_open (...) at register_cdev.c:26
(gdb) next
(gdb) print count
$5 = 2
```

`atomic_inc_return()` is what makes this number trustworthy even if
two opens happened at genuinely the same instant on different CPUs —
worth explicitly contrasting with lab 11, which is the whole lab about
what goes wrong when a shared counter *isn't* touched atomically.

### Step 4 — `read()`: the kernel-stack buffer, and EOF

```gdb
(gdb) break register_cdev_read
(gdb) continue
```

(Both backgrounded `cat`s from step 3 will have already issued their
first `read()` — `continue` past those hits, or `delete` and reset if
the flow gets confusing; see
[`../GDB_DEBUGGING.md`](../GDB_DEBUGGING.md#two-rules-that-cause-real-confusing-looking-failures-if-missed).)

```gdb
Thread 2 hit Breakpoint N, register_cdev_read (...) at register_cdev.c:46
(gdb) next   # scnprintf() into message[]
(gdb) print message
$6 = "register_cdev kernel device\nmajor=240\nminor=0\ncontext=cat[123]\n"
(gdb) print &message
```

`message` is a plain local array — `print &message` shows an address
on the *current kernel stack*, not heap memory; it exists only for the
duration of this one `read()` call and is a different address on every
call, including from the exact same process. Step further and watch
the EOF contract:

```gdb
(gdb) next    # past the copy_to_user()
(gdb) print bytes_to_copy
(gdb) continue
```

The `cat` this is serving will call `read()` a second time after
consuming the message — catch that second hit and watch `*offset >=
message_length` evaluate true, taking the `return 0;` path
immediately: that `0` is precisely what tells `cat` "end of file, stop
reading," the same convention every regular file relies on.

## Cleanup

```gdb
(gdb) delete
(gdb) break register_cdev_exit
(gdb) continue
```
```bash
# vmb:
rmmod register_cdev
```
```gdb
Thread 2 hit Breakpoint N, register_cdev_exit () at ../../linux_mainline/include/linux/timekeeping.h:174
(gdb) next   # step forward until you're back in register_cdev.c
(gdb) print atomic_read(&open_count)
```
Actually calling `atomic_read()` hits the same live-function-call
caveat as step 2 — read the field GDB already exposes instead:
```gdb
(gdb) print open_count
$7 = {counter = 0}
```
Both backgrounded `cat`s should have already exited (EOF closes their
fd, dropping `open_count` back toward 0) — if it isn't 0 here, one of
them is still holding the device open somewhere, which `rmmod` would
otherwise still permit (this driver has no refcounting tied to
`fops.owner` beyond the module reference itself — worth comparing to
lab 12's exit path, which explicitly reasons about exactly this).

```bash
# vmb:
poweroff -f
```

## What this proves

The same `struct inode *`/`struct file *` pair travels through
`open()`, every `read()`, and `release()` for one open file description
— GDB printing `current->pid`/`current->comm` at each stop shows
directly which process is on the other end of that syscall, and the
`message[]` buffer's stack address changing between calls is direct,
visible proof that a local array is genuinely reallocated (in the
"new stack frame" sense) on every single invocation rather than
persisting between them the way `open_count` — a real global — does.
