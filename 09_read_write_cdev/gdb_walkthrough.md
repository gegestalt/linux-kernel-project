# GDB walkthrough — 09_read_write_cdev, hands-on, start to finish

`read_write_cdev.c` is the modern char-device registration sequence —
`alloc_chrdev_region()` + `cdev_init()`/`cdev_add()` + `class_create()` +
`device_create()` — four separate calls in place of module 05's single
`register_chrdev()`, with the payoff that `udev` creates
`/dev/read_write_cdev0` automatically. The debugging focus is the
read/write buffer itself: a fixed 4096-byte in-kernel buffer with two
independently-tracked sizes (`data_len`, how much has actually been
written; `BUF_SIZE`, how much exists at all) and a real seekable
position — the first module where `read()`/`write()` genuinely interact
with each other's effects on the same underlying storage.

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
cd 09_read_write_cdev
make -C /home/adiopocere/Desktop/codes/linux_mainline M=$(pwd) modules
```

## Step 2 — confirm the breakpoint targets, statically

```bash
gdb -q -batch -nx -ex "file read_write_cdev.ko" \
    -ex "info line rw_read" -ex "info line rw_open" -ex "info line rw_llseek" \
    -ex "info line read_write_cdev_init" -ex "info line read_write_cdev_exit" \
    read_write_cdev.ko
```
```
Line 76 of "read_write_cdev.c" starts at address 0x408 <rw_read> ...
Line 51 of "read_write_cdev.c" starts at address 0x6e8 <rw_open> ...
Line 152 of "read_write_cdev.c" starts at address 0x128 <rw_llseek> ...
Line 166 of "read_write_cdev.c" starts at address 0xbf0 <read_write_cdev_init> ...
Line 215 of "read_write_cdev.c" starts at address 0xb68 <read_write_cdev_exit> ...
```

All five resolve to real, independent symbols.

## Step 3 — check vermagic, copy onto the scratch disk

```bash
modinfo read_write_cdev.ko | grep vermagic
```
```bash
sudo mount -o loop /home/adiopocere/Desktop/codes/qemu-vmb/labs-disk.img /tmp/vmb-mnt
sudo mkdir -p /tmp/vmb-mnt/09_read_write_cdev
sudo cp read_write_cdev.ko /tmp/vmb-mnt/09_read_write_cdev/
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

## Step 5 — start gdb, connect

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

## Step 6 — trigger the load

The `break do_init_module` from step 5 is still armed and hasn't fired
yet — the module hasn't actually loaded until you do this:

**Pane: vmb**

```bash
insmod /mnt/labs/09_read_write_cdev/read_write_cdev.ko
```

**Pane: gdb** — this is the `do_init_module` hit from step 5, arriving now:

```
Thread 2 hit Breakpoint 1, do_init_module (mod=mod@entry=0x...) at kernel/module/main.c:3089
```

At this exact point execution is paused *before*
`read_write_cdev_init()` has run at all — `do_init_module` is what calls
into it, a few frames down. Load the module's own symbols now, set the
real breakpoint, and `continue` past `do_init_module` into it:

```
lx-symbols /home/adiopocere/Desktop/codes/linux-kernel-project
```

## Step 7 — init: watch the four-step registration sequence itself

```
break read_write_cdev_init
```
```
continue
```

```
Thread 2 hit Breakpoint N, read_write_cdev_init () at read_write_cdev.c:166
```
```
next
```
```
print buffer
```
```
$1 = (char *) 0xffff... ""
```
```
next
```
```
print devt
```

`MAJOR()` as a live function/macro invocation may not resolve on a KGDB
target the way it does in userspace GDB — if it errors, decode it by
hand instead (it's `devt >> 20` on this kernel's `dev_t` layout):

```
print devt >> 20
```
```
next
```
```
print rw_cdev.ops
```
```
$2 = (const struct file_operations *) 0x... <rw_fops>
```

`rw_cdev.ops` pointing directly at the file-scope `rw_fops` symbol (not
just some address — GDB resolves it back to the actual symbol name) is
the concrete proof that `cdev_init(&rw_cdev, &rw_fops)` did what it
claims: bound this exact function table to this exact device.

```
next
```
```
print rw_class
```
```
next
```
```
finish
```

**Pane: vmb**

```bash
ls -la /dev/read_write_cdev0
```

## Step 8 — write: watch `data_len` grow, distinct from `BUF_SIZE`

**Pane: gdb**

```
break rw_write
```
```
continue
```

**Pane: vmb**

```bash
printf 'hello gdb' > /dev/read_write_cdev0
```

**Pane: gdb**

```
Thread 2 hit Breakpoint N, rw_write (...) at read_write_cdev.c:107
```
```
print count
```
```
$3 = 9
```
```
print data_len
```
```
$4 = 0
```
```
next
```
```
next
```
```
next
```
```
print space
```
```
$5 = 4096
```
```
next
```
```
print to_copy
```
```
$6 = 9
```
```
next
```
```
next
```
```
next
```
```
print data_len
```
```
$7 = 9
```

## Step 9 — the short-write case: prove the buffer's ceiling is real

The source comment is explicit that a short write only ever happens when
the buffer is genuinely full. Force it directly rather than trust the
comment — and note this driver does **not** implement `O_APPEND`
semantics itself (it never calls `generic_write_checks()`/inspects
`file->f_flags & O_APPEND`), so a fresh `open()` always starts `*ppos` at
0 regardless of how the fd was opened. To actually push `*ppos` up to
`BUF_SIZE`, hold **one** fd open across two writes with the shell's own
`exec`, rather than two separate redirections (which would each start
over at position 0):

**Pane: vmb**

```bash
exec 3> /dev/read_write_cdev0
```
```bash
dd if=/dev/zero bs=1 count=4096 2>/dev/null | tr '\0' 'x' >&3
```

**Pane: gdb**

```
continue
```
```
Thread 2 hit Breakpoint N, rw_write (...) at read_write_cdev.c:107
```
```
print count
```
```
$8 = 4096
```
```
continue
```

**Pane: vmb** (same fd 3, position now sits at `BUF_SIZE` from that last write)

```bash
printf 'overflow' >&3
```

**Pane: gdb**

```
Thread 2 hit Breakpoint N, rw_write (...) at read_write_cdev.c:107
```
```
print *ppos
```
```
$9 = 4096
```
```
next
```
```
finish
```
```
Value returned is $10 = -28
```

`-28` is `-ENOSPC` (`errno.h`'s value 28 for `ENOSPC`, negated per
kernel convention) — GDB shows you the literal integer the function
returned; matching it to the symbolic name is on you, the same way it
would be reading a raw strace. This is a case `rw_write` never reaches
through its own `to_copy < count` short-write path in this exact
scenario (a write starting exactly at capacity hits the earlier `*ppos
>= BUF_SIZE` check instead) — worth noticing the two different code
paths that can each produce "your write didn't fully succeed," with
different concrete `errno`s.

## Step 10 — `llseek`: a real, boundary-checked reposition

**Pane: gdb**

```
info breakpoints
```
```
delete N
```

(Use the actual `rw_write` breakpoint number — bare `delete` prompts for
confirmation, and a queued-up next command can eat that prompt instead
of actually deleting anything.)

```
break rw_llseek
```
```
continue
```

**Pane: vmb**

```bash
dd if=/dev/read_write_cdev0 bs=1 skip=4090 count=6 2>/dev/null
```

**Pane: gdb**

```
Thread 2 hit Breakpoint N, rw_llseek (file=0x..., offset=4090, whence=0) at read_write_cdev.c:152
```
```
step
```

`fixed_size_llseek()` is generic kernel code (`fs/read_write.c`), shared
by every driver with this exact "fixed-capacity, randomly addressable"
shape — stepping into it here is a quick way to see the
`SEEK_SET`/`SEEK_CUR`/`SEEK_END`/bounds-checking logic your own driver
gets for free by calling it, rather than re-implementing it.

## Step 11 — the read side: EOF exactly at `data_len`, not `BUF_SIZE`

**Pane: gdb**

```
info breakpoints
```
```
delete N
```
```
break rw_read
```
```
continue
```

**Pane: vmb**

```bash
cat /dev/read_write_cdev0
```

**Pane: gdb**

```
Thread 2 hit Breakpoint N, rw_read (...) at read_write_cdev.c:76
```
```
print *ppos
```
```
print data_len
```
```
next
```
```
print available
```
```
print to_copy
```
```
finish
```
```
continue
```
```
Thread 2 hit Breakpoint N, rw_read (...) at read_write_cdev.c:76
```
```
print *ppos
```

`cat`'s second `read()` call just hit the same breakpoint again — `*ppos`
now equals `data_len`, exactly the condition `rw_read`'s own EOF check
(`*ppos >= data_len`) is watching for, so this call returns 0 next.

## Step 12 — clean up

**`break read_write_cdev_exit` does not work if you try it directly.**
`read_write_cdev_exit` is marked `__exit`, which places it in its own
ELF section, `.exit.text`, separate from the module's regular `.text`.
`lx-symbols` relocates only a fixed, hardcoded list of extra sections
when it maps a loaded module's addresses, and `.exit.text` isn't in that
list. The naive breakpoint silently resolves to the function's raw,
unrelocated file offset instead of a real kernel address — setting it
appears to succeed with no error, it just never fires. `rmmod` completes
normally, `dmesg` shows the unload message, and GDB sits at
`Continuing.` forever having quietly missed it.

**The fix** — break on the generic kernel hook that calls into every
module's exit function:

```
info breakpoints
```
```
delete N
```
```
break __do_sys_delete_module
```
```
continue
```

**Pane: vmb**

```bash
rmmod read_write_cdev
```

**Pane: gdb**

```
advance kernel/module/main.c:863
```
```
print mod->exit
```
```
$N = (void (*)(void)) 0xffff80007c320680
```

(That address is from one real run and won't match yours — module
memory placement is random per boot regardless of `nokaslr`, which only
fixes the kernel image's own load address. Always use whatever `print
mod->exit` gives you right now.) **Do not `step` into it from here** —
with no relocated line table, GDB can't bound the function, so `step`
free-runs straight through the rest of the exit function and beyond.
`Ctrl-C` recovers you (same guest-freeze/interrupt behavior as anywhere
else in KGDB) if you've already typed it.

Register the section's real address with GDB the same way `lx-symbols`
does it for the sections it already knows about, then the normal
breakpoint resolves cleanly:

```
add-symbol-file /home/adiopocere/Desktop/codes/linux-kernel-project/09_read_write_cdev/read_write_cdev.ko -s .exit.text 0xffff80007c320680
```
```
break read_write_cdev_exit
```
```
Breakpoint N at 0xffff80007c320680: file read_write_cdev.c, line 215.
```
```
delete N
```

(The `__do_sys_delete_module` breakpoint's number.)

```
continue
```

**Pane: vmb**

```bash
rmmod read_write_cdev
```

**Pane: gdb**

```
Thread N hit Breakpoint N, read_write_cdev_exit () at read_write_cdev.c:215
```
```
215		device_destroy(rw_class, devt);
```
```
print data_len
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

`data_len` and `BUF_SIZE` are genuinely independent quantities tracked
by different mechanisms — one a compile-time constant, one runtime state
updated on every successful write — and stepping through both the "fits
fine" and "buffer is full" branches with real writes (steps 8–9) shows
exactly which condition trips which return path, down to the specific
negative `errno` value the function actually returns. `rw_cdev.ops`
resolving back to the symbol `rw_fops` (step 7) is the same kind of
direct proof module 08 got from `filp->private_data`: GDB reading a live
pointer and recognizing what it points to beats reading the
`cdev_init()` call and assuming it worked.
