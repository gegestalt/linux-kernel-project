# GDB walkthrough — 10_ioctl_basics

`ioctl_basics.c` implements one `unlocked_ioctl()` callback dispatching
four commands built from the standard `_IO`/`_IOR`/`_IOW`/`_IOWR`
macros (`ioctl_basics.h` — shared between the driver and
`ioctl_test.c`, the userspace side). Unlike `read()`/`write()`, an
`ioctl()` argument is a single `unsigned long` that's usually really a
userspace pointer in disguise, decoded entirely by convention baked
into the command number — GDB is the most direct way to watch that
decoding actually happen, argument by argument, command by command.

## Environment

```bash
cd 10_ioctl_basics
make -C /home/adiopocere/Desktop/codes/linux_mainline M=$(pwd) modules
modinfo ioctl_basics.ko | grep vermagic
```

This module also needs a userspace test program cross-built for the
guest's architecture (aarch64) and copied on alongside the module:

```bash
aarch64-linux-gnu-gcc -static -O2 -o ioctl_test ioctl_test.c
sudo mount -o loop /home/adiopocere/Desktop/codes/qemu-vmb/labs-disk.img /tmp/vmb-mnt
sudo mkdir -p /tmp/vmb-mnt/10_ioctl_basics
sudo cp ioctl_basics.ko ioctl_test /tmp/vmb-mnt/10_ioctl_basics/
sudo umount /tmp/vmb-mnt
```

(`-static` matters — the busybox initramfs has no shared-library
loader for a normal dynamically-linked binary to work against.)

## The actual command numbers, decoded once

From `ioctl_basics.h`, `IOCTL_BASICS_MAGIC` is `'k'`:

```
IOCTL_BASICS_RESET      = _IO('k', 1)                                  no payload
IOCTL_BASICS_GET_STATS  = _IOR('k', 2, struct ioctl_basics_stats)      kernel -> user
IOCTL_BASICS_SET_MODE   = _IOW('k', 3, __u32)                          user -> kernel
IOCTL_BASICS_ECHO       = _IOWR('k', 4, struct ioctl_basics_echo)      both directions
```

Each of these macros packs direction, size, magic, and number into one
32-bit integer — `switch (cmd)` in `ioctl_basics_ioctl()` is comparing
against that packed integer directly, never unpacking it at all; only
the *caller's* `ioctl(2)` glibc wrapper needs to know the encoding, to
build `cmd` in the first place. Confirm this from the driver side, live:

```gdb
(gdb) print/x IOCTL_BASICS_RESET
```
This will fail — these are preprocessor `#define`s, gone by the time
the file is compiled, not real symbols GDB's DWARF info knows about.
Print the raw `cmd` value from inside the breakpoint instead (step 2).

## tmux layout

Standard `vmb` + `gdbsess` — [`../gdb_debugging.md`](../gdb_debugging.md).

## The walkthrough

### Step 1 — load and set up

```gdb
(gdb) target remote :1234
(gdb) lx-version
(gdb) break do_init_module
(gdb) continue
```
```bash
# vmb:
insmod /mnt/labs/10_ioctl_basics/ioctl_basics.ko
dmesg | tail -2
mknod /dev/ioctl_basics0 c $(dmesg | grep -o 'major=[0-9]*' | tail -1 | cut -d= -f2) 0
```
```gdb
(gdb) lx-symbols /home/adiopocere/Desktop/codes/linux-kernel-project
(gdb) break ioctl_basics_ioctl
(gdb) continue
```

### Step 2 — run the full test program, one `continue` per command

```bash
# vmb:
chmod +x /mnt/labs/10_ioctl_basics/ioctl_test
/mnt/labs/10_ioctl_basics/ioctl_test /dev/ioctl_basics0
```

The test program issues six `ioctl()` calls in sequence
(`RESET`, `GET_STATS`, `SET_MODE upper`, `SET_MODE reverse`, an invalid
`SET_MODE`, and an unrecognized command) plus three `ECHO`s along the
way — your breakpoint fires once per call. Read `cmd`/`arg` at each
stop before stepping further:

```gdb
Thread 2 hit Breakpoint N, ioctl_basics_ioctl (file=0x..., cmd=1, arg=0) at ioctl_basics.c:74
(gdb) print/x cmd
$1 = 0x6b01
```

`0x6b01` — `0x6b` is ASCII `'k'`, the magic; `01` is the command
number — the RESET command, decoded right in the printed hex without
needing to evaluate the macro at all.

```gdb
(gdb) continue
Thread 2 hit Breakpoint N, ioctl_basics_ioctl (file=0x..., cmd=..., arg=...) at ioctl_basics.c:74
(gdb) print/x cmd     # GET_STATS - direction bits now set since it's an _IOR
```

Step into the `GET_STATS` case body specifically:

```gdb
(gdb) next            # into the switch
(gdb) next             # mutex_lock, stats.reads++, snapshot = stats
(gdb) print snapshot
$2 = {reads = 1, echoes = 0, resets = 1, mode = 0}
(gdb) print argp
$3 = (void *) 0x...    # the userspace pointer, still raw - not yet dereferenced
```

`argp` is a real userspace address at this point — `print
*(struct ioctl_basics_stats *)argp` would very likely fault or read
garbage, because it's not a kernel-mapped pointer; this is exactly why
`copy_to_user()` exists just below rather than a plain pointer
assignment. Step past it and watch the return:

```gdb
(gdb) next    # copy_to_user(argp, &snapshot, sizeof(snapshot))
(gdb) finish
```

### Step 3 — `SET_MODE`: watch the validated write path

```gdb
(gdb) continue
Thread 2 hit Breakpoint N, ioctl_basics_ioctl (...) at ioctl_basics.c:74
(gdb) print/x cmd       # SET_MODE - the _IOW one
(gdb) next               # into the switch
(gdb) next                # copy_from_user(&mode, argp, sizeof(mode))
(gdb) print mode
$4 = 1                     # IOCTL_BASICS_MODE_UPPER
(gdb) next                  # the mode > IOCTL_BASICS_MODE_REVERSE bounds check
(gdb) next                   # stats.mode = mode
```

Continue to the *invalid* `SET_MODE` call the test program makes later
(`mode = 99`) — same breakpoint, same code path, different outcome:

```gdb
(gdb) continue
...
(gdb) next    # copy_from_user
(gdb) print mode
$5 = 99
(gdb) next     # bounds check: 99 > IOCTL_BASICS_MODE_REVERSE (2) -> true
(gdb) finish
Value returned is $6 = -22    # -EINVAL
```

### Step 4 — `ECHO`: the mode transform, live, both directions

```gdb
(gdb) continue
Thread 2 hit Breakpoint N, ioctl_basics_ioctl (...) at ioctl_basics.c:74
(gdb) print/x cmd    # ECHO - the _IOWR one
(gdb) next             # copy_from_user(&req, argp, sizeof(req))
(gdb) print req.buf
$7 = "Hello, kernel!", '\000' <repeats ...>
(gdb) next              # req.buf[sizeof(req.buf)-1] = '\0' - defensive null-termination
(gdb) next               # mode = stats.mode  (this ECHO happens after SET_MODE upper)
(gdb) print mode
$8 = 1
(gdb) step                # step INTO apply_mode_transform() rather than over it
```

Verified statically: `Line 74 <ioctl_basics_ioctl>`, and
`apply_mode_transform`'s own breakable body starts wherever your
`step` above landed — check with `info line apply_mode_transform` once
you're inside it. Step through the `IOCTL_BASICS_MODE_UPPER` loop a
character or two, watching `buf[i]` change in place:

```gdb
(gdb) print buf
(gdb) next
(gdb) print buf
```

`copy_to_user(argp, &req, sizeof(req))` at the end of the `ECHO` case
sends the *same* `req.buf` — now mutated in kernel memory — back across
the boundary, which is exactly why `IOCTL_BASICS_ECHO` is declared
`_IOWR`: the same buffer genuinely travels in both directions in one
call, unlike `read()`/`write()`, which are always one-directional.

### Step 5 — the unrecognized command

```gdb
(gdb) continue
Thread 2 hit Breakpoint N, ioctl_basics_ioctl (file=0x..., cmd=..., arg=0) at ioctl_basics.c:74
(gdb) print/x cmd
$9 = 0x6b63    # _IO('k', 99) - matches nothing in the switch
(gdb) finish
Value returned is $10 = -25    # -ENOTTY
```

`-ENOTTY` (25) is the kernel-wide convention for "this ioctl command
isn't implemented by this file" — borrowed from tty ioctls historically
but used generically; your driver's `default:` case returns exactly
this, and `ioctl_test`'s own output (`"failed as expected: Inappropriate
ioctl for device"`) is just `strerror(25)`.

## Cleanup

**`break ioctl_basics_exit` does not work if you try it directly —
confirmed live.** `ioctl_basics_exit` is marked `__exit`, placing it in
its own ELF section, `.exit.text`, which `lx-symbols` never relocates
(its hardcoded section list in `scripts/gdb/linux/symbols.py` covers
`.text`/`.data`/`.rodata`/`.bss` and a few others, but not
`.init.text`/`.exit.text`). The breakpoint silently resolves to a raw,
unrelocated file offset instead of a real kernel address — no error,
it just never fires. This affects every module in this repo using the
modern `module_exit()` macro (every module except 01).

**The fix, verified live** — break on the generic kernel hook that
calls into every module's exit function, then read the real address
straight out of the kernel's own struct once you're there:

```gdb
(gdb) delete <the ioctl_basics_ioctl breakpoint's number — `info breakpoints` if unsure>
(gdb) break __do_sys_delete_module
(gdb) continue
```
```bash
# vmb:
rmmod ioctl_basics
```
```gdb
(gdb) advance kernel/module/main.c:863
(gdb) print mod->exit
$N = (void (*)(void)) 0xffff80007c320700
```

(That address is from one real run and won't match yours — module
memory placement is random per boot regardless of `nokaslr`. Always use
whatever `print mod->exit` gives you right now.) **Do not `step` into
it from here** — with no relocated line table, GDB can't bound the
function and `step` free-runs straight past it; `Ctrl-C` recovers you
if you've already tried. Register the section with GDB the way
`lx-symbols` does for the sections it already knows about, and the
normal breakpoint then resolves cleanly:

```gdb
(gdb) add-symbol-file /home/adiopocere/Desktop/codes/linux-kernel-project/10_ioctl_basics/ioctl_basics.ko -s .exit.text 0xffff80007c320700
(gdb) break ioctl_basics_exit
Breakpoint N at 0xffff80007c320700: file ioctl_basics.c, line 203.
(gdb) delete <the __do_sys_delete_module breakpoint's number>
(gdb) continue
```
```bash
# vmb:
rmmod ioctl_basics
```
```gdb
Thread N hit Breakpoint N, ioctl_basics_exit () at ioctl_basics.c:203
203		device_destroy(ioctl_class, devt);
(gdb) continue
```
```bash
# vmb:
poweroff -f
```

## What this proves

An `ioctl()` command number is a plain integer with direction/size/
magic/number bits packed in by the `_IO*` macros at *compile time on
the caller's side* — the driver never unpacks any of that, it just
`switch`es on the whole integer, which is directly visible in
`print/x cmd`'s output at every stop. And `arg`/`argp` really is a raw
userspace pointer handed to the kernel with zero automatic safety —
every single command here earns its `copy_to_user()`/`copy_from_user()`
call, and stepping up to (but not past) one shows you the address
sitting there unvalidated, exactly as dangerous as it looks until that
call actually executes.
