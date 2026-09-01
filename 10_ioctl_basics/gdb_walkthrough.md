# GDB walkthrough — 10_ioctl_basics, hands-on, start to finish

`ioctl_basics.c` implements one `unlocked_ioctl()` callback dispatching
four commands built from the standard `_IO`/`_IOR`/`_IOW`/`_IOWR` macros
(`ioctl_basics.h` — shared between the driver and `ioctl_test.c`, the
userspace side). Unlike `read()`/`write()`, an `ioctl()` argument is a
single `unsigned long` that's usually really a userspace pointer in
disguise, decoded entirely by convention baked into the command number —
GDB is the most direct way to watch that decoding actually happen,
argument by argument, command by command.

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

## Step 1 — build the module and the userspace test program

*Regular terminal (detach with `Ctrl-b d`, or a separate window).*

```bash
cd /home/adiopocere/Desktop/codes/linux-kernel-project/10_ioctl_basics
make -C /home/adiopocere/Desktop/codes/linux_mainline M=$(pwd) modules
```

This module also needs a userspace test program, cross-built for the
guest's architecture (aarch64):

```bash
aarch64-linux-gnu-gcc -static -O2 -o ioctl_test ioctl_test.c
```

(`-static` matters — the busybox initramfs has no shared-library loader
for a normal dynamically-linked binary to work against.)

## Step 2 — decode the command numbers, once, before touching a VM

From `ioctl_basics.h`, `IOCTL_BASICS_MAGIC` is `'k'`:

```
IOCTL_BASICS_RESET      = _IO('k', 1)                                  no payload
IOCTL_BASICS_GET_STATS  = _IOR('k', 2, struct ioctl_basics_stats)      kernel -> user
IOCTL_BASICS_SET_MODE   = _IOW('k', 3, __u32)                          user -> kernel
IOCTL_BASICS_ECHO       = _IOWR('k', 4, struct ioctl_basics_echo)      both directions
```

Each macro packs direction, size, magic, and number into one 32-bit
integer — `switch (cmd)` in `ioctl_basics_ioctl()` compares against that
packed integer directly, never unpacking it; only the *caller's*
`ioctl(2)` glibc wrapper needs to know the encoding, to build `cmd` in
the first place. These are preprocessor `#define`s, not real symbols —
`print/x IOCTL_BASICS_RESET` in gdb will fail once you're connected;
you'll read the raw `cmd` value from inside the breakpoint instead
(step 8).

`struct ioctl_basics_stats` — the exact shape `GET_STATS` copies to
userspace, worth `ptype`ing once before you go looking for it live in
step 9:

```bash
gdb -q -batch -nx -ex "file ioctl_basics.ko" -ex "ptype struct ioctl_basics_stats" ioctl_basics.ko
```
```
type = struct ioctl_basics_stats {
    __u64 reads;
    __u64 echoes;
    __u64 resets;
    __u32 mode;
}
```

## Step 3 — check vermagic, copy both files onto the scratch disk

```bash
modinfo ioctl_basics.ko | grep vermagic
```
```bash
sudo mount -o loop /home/adiopocere/Desktop/codes/qemu-vmb/labs-disk.img /tmp/vmb-mnt
sudo mkdir -p /tmp/vmb-mnt/10_ioctl_basics
sudo cp ioctl_basics.ko ioctl_test /tmp/vmb-mnt/10_ioctl_basics/
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

## Step 6 — load the module, load symbols

**Pane: gdb**

```
break do_init_module
```
```
continue
```

Prints `Continuing.` — switch panes.

**Pane: vmb**

```bash
insmod /mnt/labs/10_ioctl_basics/ioctl_basics.ko
```
```bash
dmesg | tail -2
```
```bash
mknod /dev/ioctl_basics0 c $(dmesg | grep -o 'major=[0-9]*' | tail -1 | cut -d= -f2) 0
```

**Pane: gdb**

```
lx-symbols /home/adiopocere/Desktop/codes/linux-kernel-project
```

## Step 7 — break on the ioctl dispatch function

**Pane: gdb**

```
break ioctl_basics_ioctl
```
```
continue
```

**Pane: vmb**

```bash
chmod +x /mnt/labs/10_ioctl_basics/ioctl_test
```
```bash
/mnt/labs/10_ioctl_basics/ioctl_test /dev/ioctl_basics0
```

The test program issues six `ioctl()` calls in sequence (`RESET`,
`GET_STATS`, `SET_MODE upper`, `SET_MODE reverse`, an invalid
`SET_MODE`, an unrecognized command) plus three `ECHO`s along the way —
your breakpoint fires once per call.

## Step 8 — `RESET`: the raw command number, decoded

**Pane: gdb**

```
Thread 2 hit Breakpoint 2, ioctl_basics_ioctl (file=0x..., cmd=1, arg=0) at ioctl_basics.c:74
```
```
print/x cmd
```
```
$1 = 0x6b01
```

`0x6b` is ASCII `'k'`, the magic byte; `01` is the command number — this
is `RESET`, decoded right in the printed hex, no macro evaluation
needed.

## Step 9 — `GET_STATS`: kernel-to-user, before `copy_to_user()`

```
continue
```
```
Thread 2 hit Breakpoint 2, ioctl_basics_ioctl (...) at ioctl_basics.c:74
```
```
print/x cmd
```

Direction bits are now set, since `GET_STATS` is an `_IOR`. Step into
the case body:

```
next
```
```
next
```
```
print snapshot
```
```
$2 = {reads = 1, echoes = 0, resets = 1, mode = 0}
```
```
print argp
```
```
$3 = (void *) 0x...
```

`argp` is a real userspace address at this point — `print *(struct
ioctl_basics_stats *)argp` would very likely fault or read garbage,
since it isn't a kernel-mapped pointer. This is exactly why
`copy_to_user()` exists just below rather than a plain pointer
assignment.

```
next
```
```
finish
```

## Step 10 — `SET_MODE`: the validated write path, both outcomes

```
continue
```
```
Thread 2 hit Breakpoint 2, ioctl_basics_ioctl (...) at ioctl_basics.c:74
```
```
print/x cmd
```
```
next
```
```
next
```
```
print mode
```
```
$4 = 1
```

`1` = `IOCTL_BASICS_MODE_UPPER`.

```
next
```
```
next
```

Continue to the *invalid* `SET_MODE` call the test program makes later
(`mode = 99`) — same breakpoint, same code path, different outcome:

```
continue
```
```
next
```
```
print mode
```
```
$5 = 99
```
```
next
```
```
finish
```
```
Value returned is $6 = -22
```

`-22` = `-EINVAL` — the bounds check (`mode >
IOCTL_BASICS_MODE_REVERSE`) caught it.

## Step 11 — `ECHO`: the same buffer, both directions

```
continue
```
```
Thread 2 hit Breakpoint 2, ioctl_basics_ioctl (...) at ioctl_basics.c:74
```
```
print/x cmd
```
```
next
```
```
print req.buf
```
```
$7 = "Hello, kernel!", '\000' <repeats ...>
```
```
next
```
```
next
```
```
print mode
```
```
$8 = 1
```
```
step
```

`step` (not `next`) here goes *into* `apply_mode_transform()` rather
than over it — it has no breakable symbol of its own, so this is the
only way in; check with `info line apply_mode_transform` once you're
inside to confirm. Step through the upper-casing loop a character or two:

```
print buf
```
```
next
```
```
print buf
```

`copy_to_user(argp, &req, sizeof(req))` at the end of the `ECHO` case
sends the *same* `req.buf` — now mutated in kernel memory — back across
the boundary, exactly why `IOCTL_BASICS_ECHO` is declared `_IOWR`: the
same buffer genuinely travels both directions in one call, unlike
`read()`/`write()`, which are always one-directional.

## Step 12 — the unrecognized command

```
continue
```
```
Thread 2 hit Breakpoint 2, ioctl_basics_ioctl (file=0x..., cmd=..., arg=0) at ioctl_basics.c:74
```
```
print/x cmd
```
```
$9 = 0x6b63
```

`_IO('k', 99)` — matches nothing in the `switch`.

```
finish
```
```
Value returned is $10 = -25
```

`-25` = `-ENOTTY` — the kernel-wide convention for "this ioctl command
isn't implemented by this file" (borrowed from tty ioctls historically,
used generically now). `ioctl_test`'s own output ("failed as expected:
Inappropriate ioctl for device") is just `strerror(25)`.

## Step 13 — the exit path

`ioctl_basics_exit` is marked `__exit`, placed in its own `.exit.text`
section, which `lx-symbols` never relocates — `break
ioctl_basics_exit` right now would silently resolve to a raw,
unrelocated file offset, not a real kernel address. Break on the generic
unload hook instead, and read the real address out of the kernel's own
struct:

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
rmmod ioctl_basics
```

**Pane: gdb**

```
Thread 2 hit Breakpoint N, __do_sys_delete_module (flags=..., name_user=...) at kernel/module/main.c:808
```
```
advance kernel/module/main.c:863
```
```
print mod->exit
```
```
$11 = (void (*)(void)) 0xffff80007c320700
```

(Your address will differ — module memory placement is random per boot
even with `nokaslr`. Use whatever `print mod->exit` gives you.)

```
add-symbol-file /home/adiopocere/Desktop/codes/linux-kernel-project/10_ioctl_basics/ioctl_basics.ko -s .exit.text 0xffff80007c320700
```
```
y
```
```
break ioctl_basics_exit
```
```
Breakpoint N at 0xffff80007c320700: file ioctl_basics.c, line 203. (2 locations)
```

Two locations — `N.1` is the old broken raw-offset one, `N.2` is the
newly-relocated real one:

```
disable N.1
```
```
continue
```

**Pane: vmb**

```bash
rmmod ioctl_basics
```

**Pane: gdb**

```
Thread N hit Breakpoint N.2, ioctl_basics_exit () at ioctl_basics.c:203
203		device_destroy(ioctl_class, devt);
```

## Step 14 — clean up

**Pane: gdb**

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

- An `ioctl()` command number is a plain integer with direction/size/
  magic/number bits packed in by the `_IO*` macros at *compile time on
  the caller's side* — the driver never unpacks any of that, it just
  `switch`es on the whole integer, directly visible in `print/x cmd`'s
  output at every stop (steps 8–12).
- `arg`/`argp` really is a raw userspace pointer handed to the kernel
  with zero automatic safety — every command here earns its
  `copy_to_user()`/`copy_from_user()` call, and stepping up to (but not
  past) one shows the address sitting there unvalidated, exactly as
  dangerous as it looks until that call actually executes (step 9).
- `apply_mode_transform()` has no symbol of its own — GCC inlined it
  into `ioctl_basics_ioctl`'s `ECHO` case — so `step` (not `break`) is
  the only way to watch it run (step 11).
- `-ENOTTY` is the kernel-wide "not this ioctl" convention;
  `-EINVAL` means "recognized command, invalid argument" — this driver's
  own test program deliberately exercises both so you can see them
  differ (steps 10, 12).
