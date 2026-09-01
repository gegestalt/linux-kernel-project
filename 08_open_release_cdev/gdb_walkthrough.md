# GDB walkthrough — 08_open_release_cdev, hands-on, start to finish

`open_release_cdev.c` is entirely about `struct file::private_data` —
each `open()` allocates a small `struct open_context` with `kzalloc()`,
stamps it with an ID/timestamp/opener PID, and stores the pointer in
`filp->private_data`; `release()` recovers that exact same pointer from
that exact same field and frees it. This module's whole point is a
question earlier modules never had to answer: **how does `release()`
know anything about the specific `open()` call it's closing out?** The
answer is entirely mechanical, and GDB can show you the same pointer
value on both sides of that round-trip.

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
cd /home/adiopocere/Desktop/codes/linux-kernel-project/08_open_release_cdev
make -C /home/adiopocere/Desktop/codes/linux_mainline M=$(pwd) modules
```

This also builds `cdev_test`, a small userspace helper (`test.c`) that
exercises four open-flag combinations plus the `dup()` case used in
step 6 below.

## Step 2 — confirm the breakpoint targets, statically

```bash
gdb -q -batch -nx -ex "file open_release_cdev.ko" \
    -ex "info line my_open" -ex "info line my_release" \
    -ex "info line open_release_cdev_init" -ex "info line open_release_cdev_exit" \
    open_release_cdev.ko
```
```
Line 60 of "open_release_cdev.c" starts at address ... <my_open> ...
Line 128 of "open_release_cdev.c" starts at address ... <my_release> ...
Line 172 of "open_release_cdev.c" starts at address 0xb90 <open_release_cdev_init> ...
Line 189 of "open_release_cdev.c" starts at address 0xc50 <open_release_cdev_exit> ...
```

All four resolve to real, independent symbols.

## Step 3 — check vermagic, copy onto the scratch disk

```bash
modinfo open_release_cdev.ko | grep vermagic
```
```bash
sudo mount -o loop /home/adiopocere/Desktop/codes/qemu-vmb/labs-disk.img /tmp/vmb-mnt
sudo mkdir -p /tmp/vmb-mnt/08_open_release_cdev
sudo cp open_release_cdev.ko /tmp/vmb-mnt/08_open_release_cdev/
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
lx-version
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
insmod /mnt/labs/08_open_release_cdev/open_release_cdev.ko
```
```bash
dmesg | tail -3
```
```bash
mknod /dev/open_release_cdev0 c $(dmesg | grep -o 'major=[0-9]*' | tail -1 | cut -d= -f2) 0
```

**Pane: gdb**

```
lx-symbols /home/adiopocere/Desktop/codes/linux-kernel-project
```
```
break my_open
```
```
break my_release
```

## Step 6 — open: watch `private_data` go from NULL to a real pointer

```
continue
```

**Pane: vmb**

```bash
exec 3< /dev/open_release_cdev0
```

(Using the shell's own `exec 3<` rather than `cat` gives you a held-open
fd you fully control from `vmb` without a second backgrounded process —
you'll close it explicitly with `exec 3<&-` when ready for step 7.)

**Pane: gdb**

```
Thread 2 hit Breakpoint N, my_open (inode=0x..., filp=0x...) at open_release_cdev.c:60
```
```
print filp->private_data
```
```
$1 = (void *) 0x0
```

Confirmed: nothing has set it yet — this is the field's state exactly as
the VFS itself initialized it before calling into this driver.

```
next
```
```
print ctx
```
```
$2 = (struct open_context *) 0xffff...
```
```
next
```
```
print ctx->id
```
```
next
```
```
print ctx->flags
```
```
print/x ctx->mode
```
```
next
```
```
print ctx->opener_comm
```
```
next
```
```
print filp->private_data
```
```
$3 = (void *) 0xffff...
```

Now equals `ctx` from a moment ago. **Write down this address** (or keep
the gdb session open — `$2`/`$3` still reference it) — you're about to
see it again, unchanged, in a completely different function call.

## Step 7 — release: the same pointer, recovered

```
continue
```

**Pane: vmb**

```bash
exec 3<&-
```

(Closes fd 3 — triggers `->release()` since it's the last reference.)

**Pane: gdb**

```
Thread 2 hit Breakpoint N, my_release (inode=0x..., filp=0x...) at open_release_cdev.c:128
```
```
print filp->private_data
```
```
$4 = (struct open_context *) 0xffff...
```

Identical to `$2`/`$3` above. This is the entire mechanism, made
undeniable: `filp->private_data` in `my_release()` is the *exact same
pointer value* `my_open()` wrote into that field earlier — not a copy,
not something re-derived from `inode`, literally the same bits recovered
from the same `struct file` that persisted across every syscall against
this one open file description.

```
print ((struct open_context *)filp->private_data)->id
```
```
next
```
```
next
```
```
print lifetime_ns
```
```
next
```
```
print active
```
```
next
```

`kfree(ctx)` just ran, immediately followed by the source's own
`filp->private_data = NULL;` — this defensive nulling is what makes a
hypothetical second call into this driver against the same
(now-dangling) `struct file` fail safely instead of touching freed
memory, though the VFS itself won't normally hand you a second
`release()` for the same open.

## Step 8 — the `dup()` case the source comment warns about

The source's comment on `my_release()` calls out `dup()` specifically:
"closing one duplicated descriptor does not necessarily call
`->release()`." Prove it.

```
continue
```

**Pane: vmb**

```bash
exec 3< /dev/open_release_cdev0
```
```bash
exec 4<&3
```

(`fd` 4 now duplicates fd 3 — both reference the *same* open file
description.)

```bash
exec 3<&-
```

Closing one of the two produces **no** hit on `my_release` — check the
gdb pane, nothing fired. `dup()` doesn't create a new open file
description, only a new file descriptor pointing at the existing one;
the kernel's reference count on that one `struct file` only reaches zero
(triggering `->release()`) once *both* fd 3 and fd 4 are closed:

```bash
exec 4<&-
```

**Pane: gdb**

```
Thread 2 hit Breakpoint N, my_release (...) at open_release_cdev.c:128
```

*Now* it fires — on the second close, not the first, exactly matching
the comment, now demonstrated rather than taken on faith.

## Step 9 — clean up

**`break open_release_cdev_exit` accepts with no error but never
fires.** It's marked `__exit`, which places it in the `.exit.text` ELF
section, and `lx-symbols` never relocates that section. `rmmod` would
complete underneath it while GDB just sits at `Continuing.` forever —
the `print`s below would never actually run against a stopped exit
function. The fix: break on the generic kernel hook every `rmmod` goes
through instead.

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
rmmod open_release_cdev
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
$1 = (void (*)(void)) 0xffff80007c3204d0
```

(That address is from one real run — use whatever `print mod->exit`
gives you; module memory placement is random per boot even with
`nokaslr`.)

```
add-symbol-file /home/adiopocere/Desktop/codes/linux-kernel-project/08_open_release_cdev/open_release_cdev.ko -s .exit.text 0xffff80007c3204d0
```
```
y
```
```
break open_release_cdev_exit
```
```
Breakpoint N at 0x110: open_release_cdev_exit. (2 locations)
```

Disable the broken raw-offset location, keep the relocated one:

```
disable N.1
```
```
continue
```

**Pane: vmb**

```bash
rmmod open_release_cdev
```

**Pane: gdb**

```
Thread N hit Breakpoint N.2, 0xffff80007c3204d4 in cleanup_module ()
```
```
print active_opens
```
```
print next_open_id
```
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

`struct file::private_data` is nothing more than one pointer-sized slot
the VFS carries for you between every syscall against one open file
description — this walkthrough watched the identical bit pattern written
in `open()` and read back in `release()` (steps 6–7), which is strictly
stronger evidence than the source's own comments claiming it. The
`dup()` experiment (step 8) goes further: it shows that "closing a file
descriptor" and "the open file description going away" are two different
events the kernel tracks separately, and `->release()` is tied to the
second one, not the first — a distinction that matters the moment any
real program uses `dup()`/`dup2()`, `fork()`, or inherited file
descriptors across `exec()`.
