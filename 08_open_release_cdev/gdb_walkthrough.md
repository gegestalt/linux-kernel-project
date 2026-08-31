# GDB walkthrough — 08_open_release_cdev

`open_release_cdev.c` is entirely about `struct file::private_data` —
each `open()` allocates a small `struct open_context` with `kzalloc()`,
stamps it with an ID/timestamp/opener PID, and stores the pointer in
`filp->private_data`; `release()` recovers that exact same pointer from
that exact same field and frees it. This lab's whole point is a
question earlier labs never had to answer: **how does `release()` know
anything about the specific `open()` call it's closing out?** The
answer is entirely mechanical, and GDB can show you the same pointer
value on both sides of that round-trip.

## Environment

```bash
cd 08_open_release_cdev
make -C /home/adiopocere/Desktop/codes/linux_mainline M=$(pwd) modules
modinfo open_release_cdev.ko | grep vermagic
sudo mount -o loop /home/adiopocere/Desktop/codes/qemu-vmb/labs-disk.img /tmp/vmb-mnt
sudo mkdir -p /tmp/vmb-mnt/08_open_release_cdev
sudo cp open_release_cdev.ko /tmp/vmb-mnt/08_open_release_cdev/
sudo umount /tmp/vmb-mnt
```

## tmux layout

Standard `vmb` + `gdbsess` — [`../gdb_debugging.md`](../gdb_debugging.md).

## The walkthrough

### Step 1 — load, mknod, and set both breakpoints up front

```gdb
(gdb) target remote :1234
(gdb) lx-version
(gdb) break do_init_module
(gdb) continue
```
```bash
# vmb:
insmod /mnt/labs/08_open_release_cdev/open_release_cdev.ko
dmesg | tail -3
mknod /dev/open_release_cdev0 c $(dmesg | grep -o 'major=[0-9]*' | tail -1 | cut -d= -f2) 0
```
```gdb
(gdb) lx-symbols /home/adiopocere/Desktop/codes/linux-kernel-project
(gdb) break my_open
(gdb) break my_release
```

Verified: `Line 60 <my_open>`, `Line 128 <my_release>`.

### Step 2 — open: watch `private_data` go from NULL to a real pointer

```gdb
(gdb) continue
```
```bash
# vmb:
exec 3< /dev/open_release_cdev0    # opens fd 3, holds it open, doesn't read yet
```

(Using a shell's own `exec 3<` rather than `cat` gives you a held-open
fd you fully control from `vmb` without a second backgrounded process
— you'll close it explicitly with `exec 3<&-` when ready for step 3.)

```gdb
Thread 2 hit Breakpoint N, my_open (inode=0x..., filp=0x...) at open_release_cdev.c:60
(gdb) print filp->private_data
$1 = (void *) 0x0
```

Confirmed: nothing has set it yet — this is the field's state exactly
as the VFS itself initialized it before calling into this driver.

```gdb
(gdb) next    # kzalloc(sizeof(*ctx), GFP_KERNEL)
(gdb) print ctx
$2 = (struct open_context *) 0xffff...
(gdb) next     # ctx->id = atomic64_inc_return(&next_open_id)
(gdb) print ctx->id
(gdb) next      # ctx->opened_ns, ctx->minor, ctx->flags, ctx->mode - step past each
(gdb) print ctx->flags
(gdb) print/x ctx->mode
(gdb) next        # get_task_comm - past this, ctx->opener_comm is filled
(gdb) print ctx->opener_comm
(gdb) next         # filp->private_data = ctx  <- the actual round-trip write
(gdb) print filp->private_data
$3 = (void *) 0xffff...    # now equals `ctx` from a moment ago
```

**Write down this address** (or keep the gdb session open —
`$2`/`$3` still reference it) — you're about to see it again,
unchanged, in a completely different function call.

### Step 3 — release: the same pointer, recovered

```gdb
(gdb) continue
```
```bash
# vmb:
exec 3<&-    # close fd 3 - triggers ->release() since it's the last reference
```
```gdb
Thread 2 hit Breakpoint N, my_release (inode=0x..., filp=0x...) at open_release_cdev.c:128
(gdb) print filp->private_data
$4 = (struct open_context *) 0xffff...   # identical to $2/$3 above
(gdb) print ((struct open_context *)filp->private_data)->id
```

This is the entire mechanism, made undeniable: `filp->private_data`
in `my_release()` is the *exact same pointer value* `my_open()` wrote
into that field earlier — not a copy, not something re-derived from
`inode`, literally the same bits recovered from the same `struct file`
that persisted across every syscall against this one open file
description.

```gdb
(gdb) next     # ctx = filp->private_data
(gdb) next      # lifetime_ns = ktime_get_ns() - ctx->opened_ns
(gdb) print lifetime_ns
(gdb) next       # atomic_dec_return(&active_opens)
(gdb) print active
(gdb) next        # kfree(ctx)
```

Print `filp->private_data` again immediately *after* the source's own
`filp->private_data = NULL;` line — this defensive nulling is what
makes a hypothetical second call into this driver against the same
(now-dangling) `struct file` fail safely instead of touching freed
memory, though the VFS itself won't normally hand you a second
`release()` for the same open.

### Step 4 — the `dup()` case the source comment warns about

The source's comment on `my_release()` calls out `dup()` specifically:
"closing one duplicated descriptor does not necessarily call
`->release()`." Prove it:

```gdb
(gdb) continue
```
```bash
# vmb:
exec 3< /dev/open_release_cdev0
exec 4<&3        # dup fd 3 onto fd 4 - both now reference the SAME open file description
exec 3<&-         # close one of the two - watch whether my_release fires
```

It won't — `my_open` fired once (one hit), and closing fd 3 alone
produces **no** hit on `my_release`, because `dup()` doesn't create a
new open file description, only a new file descriptor pointing at the
existing one; the kernel's reference count on that one `struct file`
only reaches zero (triggering `->release()`) once *both* fd 3 and fd 4
are closed:

```bash
# vmb:
exec 4<&-
```
```gdb
Thread 2 hit Breakpoint N, my_release (...) at open_release_cdev.c:128
```

*Now* it fires — on the second close, not the first, exactly matching
the comment, now demonstrated rather than taken on faith.

## Cleanup

```gdb
(gdb) delete
(gdb) break open_release_cdev_exit
(gdb) continue
```
```bash
# vmb:
rmmod open_release_cdev
```
```gdb
(gdb) print active_opens
(gdb) print next_open_id
```
```bash
# vmb:
poweroff -f
```

## What this proves

`struct file::private_data` is nothing more than one pointer-sized
slot the VFS carries for you between every syscall against one open
file description — this walkthrough watched the identical bit pattern
written in `open()` and read back in `release()`, which is strictly
stronger evidence than the source's own comments claiming it. The
`dup()` experiment in step 4 goes further: it shows that "closing a
file descriptor" and "the open file description going away" are two
different events the kernel tracks separately, and `->release()` is
tied to the second one, not the first — a distinction that matters the
moment any real program uses `dup()`/`dup2()`, `fork()`, or
inherited file descriptors across `exec()`.
