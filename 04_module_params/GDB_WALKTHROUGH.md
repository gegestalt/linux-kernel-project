# GDB walkthrough — 04_module_params

`module_params.c` demonstrates five different `module_param*()` flavors
(`charp`, writable `uint`, writable `bool`, a renamed `int` via
`module_param_named()`, and a fixed-size array via
`module_param_array()`). The debugging angle here is different from
every lab before it: there is almost no *control flow* worth stepping
through — the interesting question isn't "what does this code do," it's
"where do these variables' values actually come from, and when." GDB
answers that more convincingly than reading the source, because you can
watch the values already sitting in memory *before* `module_params_init()`
has executed a single line of its own body.

## Environment

```bash
cd 04_module_params
make -C /home/adiopocere/Desktop/codes/linux_mainline M=$(pwd) modules
modinfo module_params.ko | grep vermagic
sudo mount -o loop /home/adiopocere/Desktop/codes/qemu-vmb/labs-disk.img /tmp/vmb-mnt
sudo mkdir -p /tmp/vmb-mnt/04_module_params
sudo cp module_params.ko /tmp/vmb-mnt/04_module_params/
sudo umount /tmp/vmb-mnt
```

## tmux layout

Standard `vmb` + `gdbsess` pair — see
[`../GDB_DEBUGGING.md`](../GDB_DEBUGGING.md). `target remote :1234`,
`lx-version`, `break do_init_module`, `continue`.

## The walkthrough

### Step 1 — parameters are already set before your breakpoint fires

This time, load the module *with* explicit parameters, so there's
something worth inspecting:

```bash
# vmb:
insmod /mnt/labs/04_module_params/module_params.ko \
  greeting="hi from gdb" repeat_count=3 verbose=1 log_level=5 primes=11,13,17
```

```gdb
Thread 2 hit Breakpoint 1, do_init_module (mod=mod@entry=0x...) at kernel/module/main.c:3089
(gdb) lx-symbols /home/adiopocere/Desktop/codes/linux-kernel-project
(gdb) break module_params_init
```

Verified statically:

```
$ gdb -q -batch -nx -ex "file module_params.ko" -ex "info line module_params_init" module_params.ko
Line 147 of "module_params.c" starts at address 0x888 <module_params_init> and ends at 0x8ac <module_params_init+36>.
```

```gdb
(gdb) continue
Thread 2 hit Breakpoint 2, module_params_init () at module_params.c:147
147     char primes_buf[32];
```

**Before executing a single line of `module_params_init`'s own body**,
print the module-level globals directly — `insmod` already parsed and
stored them, ahead of your breakpoint, ahead of anything the function
itself does:

```gdb
(gdb) print greeting
$1 = 0x... "hi from gdb"
(gdb) print repeat_count
$2 = 3
(gdb) print verbose
$3 = true
(gdb) print dbg_level
$4 = 5
(gdb) print primes
$5 = {11, 13, 17, 7}
(gdb) print primes_count
$6 = 3
```

Two things worth actually noticing in that output: `primes` only got
the first 3 elements overwritten (`11, 13, 17`) — the array's original
compile-time default `{2, 3, 5, 7}` still owns index 3, since you only
supplied three comma-separated values. And `primes_count` — `3`, not
`4` — is populated by the `module_param_array()` macro machinery
itself, specifically to tell your code how many of the array's slots
were *actually* supplied at load time versus left at their default.
Neither of those facts is visible from reading `module_params.c` alone;
they're a property of what `insmod` was invoked with, on this
particular load.

### Step 2 — where the values actually live: `/sys/module/.../parameters/`

Confirm the same values GDB just showed you are simultaneously visible
from userspace, proving `print greeting` isn't showing you some
debugger-internal copy — it's the live C variable, and the sysfs files
are reading from that exact same memory:

```gdb
(gdb) finish
```
```bash
# vmb:
cat /sys/module/module_params/parameters/greeting
cat /sys/module/module_params/parameters/repeat_count
cat /sys/module/module_params/parameters/log_level    # renamed from dbg_level - module_param_named()
cat /sys/module/module_params/parameters/primes
```

`log_level` is the interesting one: the sysfs filename is `log_level`,
but the C variable GDB knows about is `dbg_level` — `print log_level`
in GDB would fail (`No symbol "log_level" in current context`), because
`module_param_named(log_level, dbg_level, int, 0444)` only renames the
*sysfs* file, not the C identifier. This is a real, occasionally
confusing gap between what a driver's userspace-facing name is and what
its debugger-facing name is — worth hitting once here rather than
first discovering it while debugging something that matters more.

### Step 3 — watch a writable parameter change live, from a `read()` call already in progress

`repeat_count` and `verbose` are `0644` — writable while loaded, with
no explicit "on change" callback (see the comment in the source: the
sysfs core just stores the new value directly into the variable).
`module_params_read()` re-reads `repeat_count` fresh on every call
rather than caching it, which is what makes this demonstrable:

```gdb
(gdb) break module_params_read
(gdb) continue
```
```bash
# vmb:
cat /dev/module_params_demo
```
```gdb
Thread 2 hit Breakpoint N, module_params_read (...) at module_params.c:96
(gdb) next    # past `reps = repeat_count;`
(gdb) print reps
$7 = 3
```

Leave this breakpoint stopped here — GDB, and therefore the whole
guest, is frozen mid-`read()`. Switch to a **second** shell on the
guest if you have one (or just note the value and `continue` first),
change the parameter, then read again on a fresh call:

```bash
# vmb, a second time, after continuing past the first read:
echo 8 | tee /sys/module/module_params/parameters/repeat_count
cat /dev/module_params_demo
```
```gdb
(gdb) continue
Thread 2 hit Breakpoint N, module_params_read (...) at module_params.c:96
(gdb) next
(gdb) print reps
$8 = 8
(gdb) next            # `if (reps > MAX_REPEAT) reps = MAX_REPEAT;`
(gdb) print reps
$9 = 8
```

`MAX_REPEAT` is 8 in this source, so 8 doesn't get clamped — try
writing `20` instead and watch `reps` become `8` after that same
`next`, live proof of the clamp actually executing rather than just
being present in the source.

## Cleanup

```gdb
(gdb) delete
(gdb) break module_params_exit
(gdb) continue
```
```bash
# vmb:
rmmod module_params
poweroff -f
```

## What this proves

Module parameters are populated by the kernel's module-loading code
*before* your init function's first line runs — `print`ing them at the
very top of `module_params_init()`, before stepping anywhere, is
already meaningful, unlike every earlier lab where the interesting
state only existed after some code executed. And a writable parameter
(`0644`) is genuinely live, ordinary memory with no hidden
synchronization or callback by default — GDB proves this directly by
catching the exact moment a `read()` call picks up a value changed
from a completely separate shell, seconds after the module loaded.
