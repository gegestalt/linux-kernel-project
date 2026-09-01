# GDB walkthrough — 16_debugfs_sysfs, hands-on, start to finish

`debugfs_sysfs.c` exposes the exact same two variables (`counter`,
`enabled`) through two different interfaces at once: a hand-written
sysfs `show()`/`store()` pair (validated, ABI-stable, one file per
attribute) and a one-line `debugfs_create_u32()`/`debugfs_create_bool()`
binding directly to the variables' addresses (unvalidated, explicitly
not-an-ABI, near-zero code). The debugging angle: proving both paths
really do touch the *same* memory — not two independent copies — by
writing through one interface and reading the result back through the
callback code for the other.

Every command below says exactly which pane. One command per step,
always — paste it, wait for the prompt to come back, then the next one.

`counter_raw`/`enabled_raw` (the debugfs files bound directly to the
variables via `debugfs_create_u32()`/`debugfs_create_bool()`) have **no
callback of this driver's own to break on at all** — that's the entire
point of those two calls; debugfs's own generic read/write handlers do
the work, this driver never sees the syscall. There is genuinely nothing
to step through for those two files, which is itself the thing to
notice.

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
cd /home/adiopocere/Desktop/codes/linux-kernel-project/16_debugfs_sysfs
make -C /home/adiopocere/Desktop/codes/linux_mainline M=$(pwd) modules
```

## Step 2 — check vermagic, copy onto the scratch disk

```bash
modinfo debugfs_sysfs.ko | grep vermagic
```
```bash
sudo mount -o loop /home/adiopocere/Desktop/codes/qemu-vmb/labs-disk.img /tmp/vmb-mnt
sudo mkdir -p /tmp/vmb-mnt/16_debugfs_sysfs
sudo cp debugfs_sysfs.ko /tmp/vmb-mnt/16_debugfs_sysfs/
sudo umount /tmp/vmb-mnt
```

## Step 3 — boot the guest

**Pane: vmb**

```bash
qemu-system-aarch64 -M virt -cpu max -m 1024 -smp 2 \
  -kernel /home/adiopocere/Desktop/codes/linux_mainline/arch/arm64/boot/Image \
  -initrd /home/adiopocere/Desktop/codes/qemu-vmb/initramfs.cpio.gz \
  -drive file=/home/adiopocere/Desktop/codes/qemu-vmb/labs-disk.img,if=virtio,format=raw \
  -append "console=ttyAMA0 rdinit=/init nokaslr" -nographic -s
```

Wait for `=== VM B (QEMU) ready ===` and `~ #`.

## Step 4 — start gdb, connect

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

## Step 5 — break on the load entry point

**Pane: gdb**

```
break do_init_module
```
```
continue
```

Prints `Continuing.` — switch panes.

## Step 6 — load and confirm both interfaces exist

**Pane: vmb**

```bash
insmod /mnt/labs/16_debugfs_sysfs/debugfs_sysfs.ko
```

**Pane: gdb**

```
Thread 2 hit Breakpoint 1, do_init_module (mod=mod@entry=0x...) at kernel/module/main.c:3089
```
```
lx-symbols /home/adiopocere/Desktop/codes/linux-kernel-project
```

Symbols are loaded but `debugfs_sysfs_init()` hasn't run yet — `insmod`
is still parked in `do_init_module`. Break on it now and step through the
one design decision this module's own source comment calls out:

```
break debugfs_sysfs_init
```
```
continue
```
```
Thread 2 hit Breakpoint 2, debugfs_sysfs_init () at debugfs_sysfs.c:171
```
```
next
```
```
next
```

That second `next` steps over `sysfs_create_group(demo_kobj,
&sysfs_attr_group)` (`debugfs_sysfs.c:178`) — the one *checked* call in
this function; a real failure here unwinds via `kobject_put()` and
returns early. Continue into the block right after it:

```
next
```
```
print demo_debugfs_dir
```

That's `debugfs_create_dir("debugfs_sysfs_demo", NULL)` — `demo_debugfs_dir`
now holds whatever it returned, checked or not:

```
next
```
```
next
```
```
next
```

Three more `next`s step over `debugfs_create_u32("counter_raw", ...)`,
`debugfs_create_bool("enabled_raw", ...)`, and
`debugfs_create_file("info", ...)` — `debugfs_sysfs.c:196-198` — **none
of their return values are checked or even stored anywhere.** Per this
module's own source comment right above them: on a kernel without
`CONFIG_DEBUG_FS`, or if debugfs failed to mount, these become harmless
no-ops (some return `NULL`, some an `ERR_PTR` — the debugfs helper
functions all tolerate being handed either as a later call's `parent`).
The driver's actual functionality — the sysfs interface you just watched
get set up two `next`s ago — never depends on any of this having
succeeded. Confirm `demo_debugfs_dir` is still whatever it was set to,
unaffected by whether the files under it actually got created:

```
print demo_debugfs_dir
```

```
finish
```

`finish` only returns you to `debugfs_sysfs_init()`'s caller — the module
isn't done loading yet, `insmod` in `vmb` is still blocked. Let it
actually finish:

```
continue
```

**Pane: vmb**

```bash
ls /sys/kernel/debugfs_sysfs_demo/
```
```bash
ls /sys/kernel/debug/debugfs_sysfs_demo/
```

The second `ls` may print nothing or fail if `debugfs` isn't mounted in
your initramfs — mount it explicitly first if so:

```bash
mount -t debugfs debugfs /sys/kernel/debug 2>/dev/null
```
```bash
ls /sys/kernel/debug/debugfs_sysfs_demo/
```

## Step 7 — sysfs write, debugfs read: same underlying byte

`enabled_store` is a very common name for a `kobj_attribute` toggle
handler — `grep -w enabled_store /proc/kallsyms` on this kernel turns up
several *other*, unrelated functions sharing that exact name. GDB will
either prompt you to pick which one or land on one that isn't this
driver's — if `break enabled_store` below behaves strangely, that's why;
use `break debugfs_sysfs.c:84` instead to disambiguate by location.

**Pane: gdb**

```
break enabled_store
```
```
continue
```

**Pane: vmb**

```bash
echo 0 | tee /sys/kernel/debugfs_sysfs_demo/enabled
```

**Pane: gdb**

```
Thread 2 hit Breakpoint 2, enabled_store (...) at debugfs_sysfs.c:84
```
```
next
```

(`kstrtobool(buf, &value)`.)

```
print value
```
```
$1 = false
```
```
next
```

(`mutex_lock(&data_lock)`.)

```
next
```

(`enabled = value` — the actual write.)

```
print enabled
```
```
$2 = false
```
```
finish
```

**What this shows:** now read it back through the **debugfs** path —
not sysfs, and not this driver's own code at all, since
`debugfs_create_bool()` installs a generic debugfs read handler that
dereferences the bound address directly.

**Pane: vmb**

```bash
cat /sys/kernel/debug/debugfs_sysfs_demo/enabled_raw
```

Expect `N` (or `0`, depending on this kernel's debugfs bool file
formatting) — the same value `enabled_store` just wrote, read through a
code path this driver never runs a single instruction of.

## Step 8 — confirm no driver code runs for that read, with a watchpoint

**Pane: gdb**

```
watch enabled
```
```
continue
```

**Pane: vmb**

```bash
cat /sys/kernel/debug/debugfs_sysfs_demo/enabled_raw
```

A pure read — should **not** trip the watchpoint.

```bash
echo Y > /sys/kernel/debug/debugfs_sysfs_demo/enabled_raw
```

**Pane: gdb**

```
Thread 2 hit Hardware watchpoint 2: enabled
Old value = false
New value = true
```

**What this shows:** the `watch` fires on the **write** through
`enabled_raw` (proving debugfs really does modify the same memory
address sysfs uses) but stays silent on the preceding read — a
watchpoint only trips on writes to the watched location by default in
GDB, exactly the right tool to distinguish "this touched the variable"
from "this only read it" without needing a breakpoint in code that, for
the read case, doesn't even belong to this driver.

```
bt
```

**What this shows:** the backtrace lands inside generic debugfs/VFS
write-handling code, not `debugfs_sysfs.c` — direct proof that `enabled`
changed without this driver's own `enabled_store()` ever running for
this particular write.

## Step 9 — `counter_raw`: unlocked, unlike the sysfs path

`enabled_store`/`counter_show` both take `data_lock` before touching
their variable; `debugfs_create_u32("counter_raw", ...)` does not — it
can't, since it never runs any of this driver's code at all.

**Pane: gdb**

```
delete
```
```
y
```

(Bare `delete` with no argument deletes *every* breakpoint/watchpoint,
but first asks `Delete all breakpoints? (y or n)` — naming a specific
number instead skips the prompt if you'd rather keep others armed. Same
reasoning applies to the `delete`s below.)

```
break increment_store
```
```
continue
```

**Pane: vmb**

```bash
echo 1 | tee /sys/kernel/debugfs_sysfs_demo/increment
```

**Pane: gdb**

```
Thread 2 hit Breakpoint 3, increment_store (...) at debugfs_sysfs.c:107
```
```
next
```

(`mutex_lock(&data_lock)`.)

```
next
```

(`counter++`.)

```
print counter
```
```
$3 = 1
```
```
next
```

(`mutex_unlock`.)

**What this shows:** now write directly through the unlocked debugfs
path and read it back through the locked sysfs `counter_show`.

**Pane: vmb**

```bash
echo 100 > /sys/kernel/debug/debugfs_sysfs_demo/counter_raw
```
```bash
cat /sys/kernel/debugfs_sysfs_demo/counter
```

**Pane: gdb**

```
print counter
```
```
$4 = 100
```

**What this shows:** `counter_show()`'s own `mutex_lock(&data_lock)` did
nothing to prevent this — it protects sysfs readers/writers from *each
other*, not from debugfs entirely bypassing the lock altogether. This is
a real, structural gap the source comment names directly ("writing to
`counter_raw` here overwrites `counter` directly, no logging, no
bounds, no semantics beyond 'poke this memory'") — now demonstrated as
an actual value jump you watched happen, not a hypothetical.

## Step 10 — `info_read`: the debugfs file that *is* real driver code

Not every debugfs file skips this driver's own code — `info` was
registered via `debugfs_create_file("info", 0444, ..., &info_fops)`, a
real `file_operations` struct with a real `read` callback, just like any
other char device.

**Pane: gdb**

```
delete
```
```
y
```
```
break info_read
```
```
continue
```

**Pane: vmb**

```bash
cat /sys/kernel/debug/debugfs_sysfs_demo/info
```

**Pane: gdb**

```
Thread 2 hit Breakpoint 4, info_read (...) at debugfs_sysfs.c:144
```
```
next
```

(`mutex_lock(&data_lock)` — this one DOES take the lock.)

```
next
```
```
print c
```
```
print e
```

**What this shows:** contrast this directly against step 9's
`counter_raw`. Same `/sys/kernel/debug/debugfs_sysfs_demo/` directory,
but `info` goes through real, breakable, lock-respecting driver code
while `counter_raw`/`enabled_raw` never touch a line of
`debugfs_sysfs.c` at all — two files sitting side by side in the same
debugfs directory, with fundamentally different amounts of code (and
safety) behind them.

## Step 11 — the exit path: `__exit`-section relocation gotcha

`debugfs_sysfs_exit` is marked `__exit`, placing it in its own ELF
section, `.exit.text`, which `lx-symbols` never relocates (its hardcoded
section list in `scripts/gdb/linux/symbols.py` doesn't include
`.init.text`/`.exit.text`). A direct `break debugfs_sysfs_exit` right
now would silently resolve to a raw, unrelocated file offset — no error,
it just never fires. This affects every module in this repo using the
modern `module_exit()` macro (every module except 01).

**Pane: gdb**

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
rmmod debugfs_sysfs
```

**Pane: gdb**

```
advance kernel/module/main.c:863
```
```
print mod->exit
```
```
$5 = (void (*)(void)) 0xffff80007c320540
```

(That address is from one real run and won't match yours — module
memory placement is random per boot regardless of `nokaslr`. Always use
whatever `print mod->exit` gives you right now. **Do not `step` into it
from here** — with no relocated line table GDB can't bound the function
and `step` free-runs straight past it; `Ctrl-C` recovers you.)

```
add-symbol-file /home/adiopocere/Desktop/codes/linux-kernel-project/16_debugfs_sysfs/debugfs_sysfs.ko -s .exit.text 0xffff80007c320540
```
```
y
```
```
break debugfs_sysfs_exit
```
```
Breakpoint 6 at 0xffff80007c320540: file debugfs_sysfs.c, line 207.
```
```
delete
```
```
y
```
```
continue
```

**Pane: vmb**

```bash
rmmod debugfs_sysfs
```

**Pane: gdb**

```
Thread 2 hit Breakpoint 6, debugfs_sysfs_exit () at debugfs_sysfs.c:207
207		debugfs_remove_recursive(demo_debugfs_dir);
```
```
next
```

(`debugfs_remove_recursive(demo_debugfs_dir)`.)

```
continue
```

## Step 12 — clean up

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

"Two interfaces to the same state" is not a metaphor here — a
watchpoint on `enabled` tripping from a write that never entered this
driver's own `enabled_store()` (step 8) is direct, mechanical proof that
sysfs and debugfs really do share one underlying byte, not two
synchronized copies. And the sysfs mutex genuinely only protects sysfs's
own callers from each other — debugfs's raw bindings bypass it
completely (step 9), which is exactly the tradeoff the source comment
describes as "that convenience is also the danger," now backed by a
value you watched jump from 1 to 100 with no lock in the way at all.
