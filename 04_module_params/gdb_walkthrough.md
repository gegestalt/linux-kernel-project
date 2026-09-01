# GDB walkthrough — 04_module_params, hands-on, start to finish

`module_params.c` demonstrates five `module_param*()` flavors (`charp`,
writable `uint`, writable `bool`, a renamed `int` via
`module_param_named()`, and a fixed-size array via
`module_param_array()`). The debugging angle here differs from every
earlier module: there's almost no *control flow* worth stepping through
— the interesting question is "where do these variables' values actually
come from, and when." GDB answers that more convincingly than reading
the source, because you can watch the values already sitting in memory
*before* `module_params_init()` has executed a single line of its own
body.

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

*Regular terminal.*

```bash
cd /home/adiopocere/Desktop/codes/linux-kernel-project/04_module_params
make -C /home/adiopocere/Desktop/codes/linux_mainline M=$(pwd) modules
```

## Step 2 — verify the breakpoint targets, statically

```bash
gdb -q -batch -nx -ex "file module_params.ko" \
    -ex "info line module_params_init" -ex "info line module_params_exit" \
    -ex "info line module_params_read" -ex "ptype primes" -ex "ptype greeting" \
    module_params.ko
```
```
Line 147 of "module_params.c" starts at address 0x888 <module_params_init> and ends at 0x8ac <module_params_init+36>.
Line 177 of "module_params.c" starts at address 0x838 <module_params_exit> and ends at 0x840 <module_params_exit+8>.
Line 96 of "module_params.c" starts at address 0x88 <module_params_read> and ends at 0xac <module_params_read+36>.
type = int [4]
type = char *
```

## Step 3 — check vermagic, copy onto the scratch disk

```bash
modinfo module_params.ko | grep vermagic
```
```
vermagic: 7.2.0-kgdb-debug+ SMP preempt mod_unload modversions aarch64
```
```bash
sudo mount -o loop /home/adiopocere/Desktop/codes/qemu-vmb/labs-disk.img /tmp/vmb-mnt
sudo mkdir -p /tmp/vmb-mnt/04_module_params
sudo cp module_params.ko /tmp/vmb-mnt/04_module_params/
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

## Step 6 — break on the load entry point

**Pane: gdb**

```
break do_init_module
```
```
continue
```

Switch panes.

## Step 7 — load it *with* explicit parameters, so there's something to inspect

**Pane: vmb**

```bash
insmod /mnt/labs/04_module_params/module_params.ko greeting="hi from gdb" repeat_count=3 verbose=1 log_level=5 primes=11,13,17
```

## Step 8 — load symbols, break inside init

**Pane: gdb**

```
Thread 2 hit Breakpoint 1, do_init_module (mod=mod@entry=0x...) at kernel/module/main.c:3089
```
```
lx-symbols /home/adiopocere/Desktop/codes/linux-kernel-project
```

**`break module_params_init` does not work here — confirmed live twice,
don't trust it.** `module_params_init` is `__init`, placed in
`.init.text`. `lx-symbols` relocates only a fixed, hardcoded list of
sections (`scripts/gdb/linux/symbols.py`'s `_section_arguments()`:
`.data`, `.data..read_mostly`, `.rodata`, `.bss`, `.text.hot`,
`.text.unlikely`) — `.init.text` isn't one of them, the exact same root
cause already documented for `.exit.text` below in step 12, just hitting
the *load* path instead of the unload path this time. `break
module_params_init` right now would be accepted with no error and
silently resolve to a tiny, bogus, unrelocated file offset — confirmed
live, this exact module: `0xd8`, nowhere near a real `0xffff8000...`
kernel address — it would never actually fire; `insmod` would run
straight through to completion with nothing caught.

The fix mirrors step 12's exit-path fix exactly, using `mod->init`
instead of `mod->exit` — you're still stopped inside `do_init_module`
right now, before `do_one_initcall(mod->init)` has run, so `mod->init`
is already the module's real, live init-function address:

```
print mod->init
```
```
$1 = (int (*)(void)) 0xffff80007c328068
```

(That address is from one real, live-verified run — module memory
placement is random per boot even with `nokaslr`. Use whatever `print
mod->init` gives you next.)

```
add-symbol-file /home/adiopocere/Desktop/codes/linux-kernel-project/04_module_params/module_params.ko -s .init.text 0xffff80007c328068
```
```
y
```
```
break module_params_init
```
```
Breakpoint 2 at 0xd8: module_params_init. (2 locations)
```

Two locations — `2.1` is the same old broken raw-offset one (`0xd8`,
matching what plain `break module_params_init` gave a moment ago —
confirming it really is the identical bogus address, not a new one),
`2.2` is the newly-relocated real one:

```
info breakpoints
```
```
2.1                         y   0x00000000000000d8 in module_params_init at module_params.c:147
2.2                         y   0xffff80007c328078 <init_module+8>
```

```
disable 2.1
```
```
continue
```
```
Thread 2 hit Breakpoint 2.2, 0xffff80007c328078 in init_module ()
```

Reports as `init_module`, not `module_params_init` — the same alias
mechanics documented in module 02's walkthrough (`module_init()`
generates a hard alias to the legacy name; both point at the identical
address, GDB just picked one label for this particular PC). Confirmed
live past this point too — `print greeting` here correctly reads back
whatever value `insmod` was given, before `module_params_init`'s own
body has executed a line.

## Step 9 — print the parameters before the function has run a single line

**Pane: gdb**

```
print greeting
```
```
$2 = 0x... "hi from gdb"
```
```
print repeat_count
```
```
$3 = 3
```
```
print verbose
```
```
$4 = true
```
```
print dbg_level
```
```
$5 = 5
```
```
print primes
```
```
$6 = {11, 13, 17, 7}
```
```
print primes_count
```
```
$7 = 3
```

**What this shows:** none of `module_params_init`'s own body has run
yet — you're stopped on line 147, its first executable line, before even
that runs. Every one of these values is already correct because
`insmod` parsed and stored them *before* calling into the module at all.
Two things worth noticing: `primes` only has its first 3 elements
overwritten (`11, 13, 17`) — the array's compile-time default `{2, 3, 5,
7}` still owns index 3, since only three comma-separated values were
supplied. And `primes_count` reads `3`, not `4` — populated by the
`module_param_array()` macro machinery itself, specifically to tell your
code how many slots were *actually* supplied versus left at default.
Neither fact is visible from reading the source alone.

## Step 10 — confirm the same values from userspace

```
finish
```

**Pane: vmb**

```bash
cat /sys/module/module_params/parameters/greeting
```
```bash
cat /sys/module/module_params/parameters/repeat_count
```
```bash
cat /sys/module/module_params/parameters/log_level
```
```bash
cat /sys/module/module_params/parameters/primes
```

**What this shows:** the sysfs files read the exact same memory GDB just
printed — proof `print greeting` wasn't showing a debugger-internal
copy. `log_level` is the interesting one: the sysfs filename is
`log_level`, but the C variable GDB knows about is `dbg_level` — `print
log_level` in GDB would fail (`No symbol "log_level" in current
context`), because `module_param_named(log_level, dbg_level, int,
0444)` only renames the *sysfs* file, not the C identifier.

## Step 11 — watch a writable parameter change live, mid-`read()`

`repeat_count`/`verbose` are `0644` — writable while loaded, with no
"on change" callback (the sysfs core just stores the new value directly
into the variable). `module_params_read()` re-reads `repeat_count` fresh
on every call, which is what makes this demonstrable:

**Pane: gdb**

```
break module_params_read
```
```
continue
```

**Pane: vmb**

```bash
cat /dev/module_params_demo
```

**Pane: gdb**

```
Thread 2 hit Breakpoint N, module_params_read (...) at module_params.c:96
```
```
next
```
```
print reps
```
```
$8 = 3
```

Leave this breakpoint stopped here — GDB, and the whole guest, is frozen
mid-`read()`.

```
continue
```

**Pane: vmb**

```bash
echo 8 | tee /sys/module/module_params/parameters/repeat_count
```
```bash
cat /dev/module_params_demo
```

**Pane: gdb**

```
Thread 2 hit Breakpoint N, module_params_read (...) at module_params.c:96
```
```
next
```
```
print reps
```
```
$9 = 8
```

**What this shows:** the value changed from a completely separate shell
command, seconds ago, with the module never reloaded — a `0644`
parameter really is live, ordinary memory. Continue stepping to see the
clamp:

```
next
```
```
print reps
```
```
$10 = 8
```

`MAX_REPEAT` is 8 in this source, so 8 isn't clamped. Try writing `20`
via sysfs and repeat this step — `reps` becomes `8` after that same
`next`, live proof the clamp actually executes, not just exists in the
source.

## Step 12 — clean up: the `__exit` relocation gotcha, again

`module_params_exit` is `__exit`, placed in `.exit.text`, which
`lx-symbols` never relocates — the same underlying cause documented in
[02_better_hello's walkthrough](../02_better_hello/gdb_walkthrough.md#step-11--the-exit-path-where-it-actually-differs-from-module-01).
`break module_params_exit` right now would accept with no error but
never fire.

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
rmmod module_params
```

**Pane: gdb**

```
Thread 1 hit Breakpoint N, __do_sys_delete_module (...) at kernel/module/main.c:808
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
$11 = (void (*)(void)) 0xffff80007c32b3c8
```

(That address is from one real run — module memory placement is random
per boot even with `nokaslr`. Use whatever `print mod->exit` gives you
next.)

```
add-symbol-file /home/adiopocere/Desktop/codes/linux-kernel-project/04_module_params/module_params.ko -s .exit.text 0xffff80007c32b3c8
```
```
y
```
```
break module_params_exit
```
```
Breakpoint N at 0x88: module_params_exit. (2 locations)
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
rmmod module_params
```

**Pane: gdb**

```
Thread 1 hit Breakpoint N.2, 0xffff80007c32b3cc in cleanup_module ()
```
```
next
```
```
misc_deregister (misc=0x... <module_params_miscdev>) at drivers/char/misc.c:285
```

`next` from `cleanup_module` lands inside `misc_deregister()` — a real,
fully-resolved vmlinux function, so `bt`/`finish` work normally there
even though `cleanup_module` itself has no line-by-line resolution:

```
finish
```

## Step 13 — clean up

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

- Module parameters are populated by the kernel's module-loading code
  *before* your init function's first line runs — `print`ing them at
  the very top of `module_params_init()`, before stepping anywhere, is
  already meaningful, unlike modules where the interesting state only
  exists after some code executes (steps 8–9).
- A writable (`0644`) parameter is genuinely live, ordinary memory with
  no hidden synchronization or callback by default — proven directly by
  catching the exact moment a `read()` call picks up a value changed
  from a completely separate shell, seconds after the module loaded
  (step 11). There's no `module_params_repeat_count_store()` function to
  break on, because `module_param()` never generates one.
- The `.exit.text` relocation gotcha from module 02 hits this module
  identically — same fix, same `add-symbol-file` technique (step 12).
