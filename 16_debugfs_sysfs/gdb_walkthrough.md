# GDB walkthrough — 16_debugfs_sysfs

`debugfs_sysfs.c` exposes the exact same two variables (`counter`,
`enabled`) through two different interfaces at once: a hand-written
sysfs `show()`/`store()` pair (validated, ABI-stable, one file per
attribute) and a one-line `debugfs_create_u32()`/`debugfs_create_bool()`
binding directly to the variables' addresses (unvalidated, explicitly
not-an-ABI, near-zero code). The debugging angle: proving both paths
really do touch the *same* memory — not two independent copies — by
writing through one interface and reading the result back through the
callback code for the other.

## Environment

```bash
cd 16_debugfs_sysfs
make -C /home/adiopocere/Desktop/codes/linux_mainline M=$(pwd) modules
modinfo debugfs_sysfs.ko | grep vermagic
sudo mount -o loop /home/adiopocere/Desktop/codes/qemu-vmb/labs-disk.img /tmp/vmb-mnt
sudo mkdir -p /tmp/vmb-mnt/16_debugfs_sysfs
sudo cp debugfs_sysfs.ko /tmp/vmb-mnt/16_debugfs_sysfs/
sudo umount /tmp/vmb-mnt
```

## tmux layout

Standard `vmb` + `gdbsess` — [`../gdb_debugging.md`](../gdb_debugging.md).

## Real, verified breakpoint targets

```
Line 57:  counter_show    (sysfs, read-only)
Line 71:  enabled_show     (sysfs)
Line 84:  enabled_store     (sysfs, read/write)
Line 107: increment_store    (sysfs, write-only)
Line 144: info_read           (debugfs)
Line 171: debugfs_sysfs_init
Line 207: debugfs_sysfs_exit
```

`counter_raw`/`enabled_raw` (the debugfs files bound directly to the
variables via `debugfs_create_u32()`/`debugfs_create_bool()`) have **no
callback of this driver's own to break on at all** — that's the entire
point of those two calls; debugfs's own generic read/write handlers do
the work, this driver never sees the syscall. There is genuinely
nothing to step through for those two files, which is itself the thing
to notice.

## The walkthrough

### Step 1 — load and confirm both interfaces exist

```gdb
(gdb) target remote :1234
(gdb) lx-version
(gdb) break do_init_module
(gdb) continue
```
```bash
# vmb:
insmod /mnt/labs/16_debugfs_sysfs/debugfs_sysfs.ko
```
```gdb
(gdb) lx-symbols /home/adiopocere/Desktop/codes/linux-kernel-project
```
```bash
# vmb:
ls /sys/kernel/debugfs_sysfs_demo/
ls /sys/kernel/debug/debugfs_sysfs_demo/
```

The second `ls` may print nothing or fail if `debugfs` isn't mounted in
your initramfs — mount it explicitly first if so:

```bash
# vmb:
mount -t debugfs debugfs /sys/kernel/debug 2>/dev/null
ls /sys/kernel/debug/debugfs_sysfs_demo/
```

### Step 2 — sysfs write, debugfs read: same underlying byte

```gdb
(gdb) break enabled_store
(gdb) continue
```
```bash
# vmb:
echo 0 | tee /sys/kernel/debugfs_sysfs_demo/enabled
```
```gdb
Thread 2 hit Breakpoint N, enabled_store (...) at debugfs_sysfs.c:84
(gdb) next   # kstrtobool(buf, &value)
(gdb) print value
$1 = false
(gdb) next    # mutex_lock(&data_lock)
(gdb) next     # enabled = value  <- the actual write
(gdb) print enabled
$2 = false
(gdb) finish
```

Now read it back through the **debugfs** path — not sysfs, and not
this driver's own code at all, since `debugfs_create_bool()` installs
a generic debugfs read handler that dereferences the bound address
directly:

```bash
# vmb:
cat /sys/kernel/debug/debugfs_sysfs_demo/enabled_raw
```

Expect `N` (or `0`, depending on this kernel's debugfs bool file
formatting) — the same value `enabled_store` just wrote, read through
a code path this driver never runs a single instruction of. If you
want to actually confirm no driver code executes for this read, set a
watchpoint instead of a breakpoint:

```gdb
(gdb) watch enabled
(gdb) continue
```
```bash
# vmb:
cat /sys/kernel/debug/debugfs_sysfs_demo/enabled_raw    # a pure read - should NOT trip the watchpoint
echo Y > /sys/kernel/debug/debugfs_sysfs_demo/enabled_raw
```

The `watch` should fire on the **write** through `enabled_raw` (proving
debugfs really does modify the same memory address sysfs uses) but
stay silent on the preceding read — a watchpoint only trips on writes
to the watched location by default in GDB, which is exactly the right
tool to distinguish "this touched the variable" from "this only read
it" without needing a breakpoint in code that, for the read case,
doesn't even belong to this driver.

```gdb
Thread 2 hit Hardware watchpoint 2: enabled
Old value = false
New value = true
(gdb) bt
```

The backtrace here lands inside generic debugfs/VFS write-handling
code, not `debugfs_sysfs.c` — direct proof that `enabled` changed
without this driver's own `enabled_store()` ever running for this
particular write.

### Step 3 — `counter_raw`: unlocked, unlike the sysfs path

`enabled_store`/`counter_show` both take `data_lock` before touching
their variable; `debugfs_create_u32("counter_raw", ...)` does not — it
can't, since it never runs any of this driver's code at all. This is
worth doing once as a comparison, not because it's dangerous in a
single-threaded manual session:

```gdb
(gdb) delete
(gdb) break increment_store
(gdb) continue
```
```bash
# vmb:
echo 1 | tee /sys/kernel/debugfs_sysfs_demo/increment
```
```gdb
Thread 2 hit Breakpoint N, increment_store (...) at debugfs_sysfs.c:107
(gdb) next   # mutex_lock(&data_lock)
(gdb) next    # counter++
(gdb) print counter
$3 = 1
(gdb) next     # mutex_unlock
```

Now write directly through the unlocked debugfs path and read it back
through the locked sysfs `counter_show`:

```bash
# vmb:
echo 100 > /sys/kernel/debug/debugfs_sysfs_demo/counter_raw
cat /sys/kernel/debugfs_sysfs_demo/counter
```
```gdb
(gdb) print counter
$4 = 100
```

`counter_show()`'s own `mutex_lock(&data_lock)` did nothing to prevent
this — it protects sysfs readers/writers from *each other*, not from
debugfs entirely bypassing the lock altogether. This is a real,
structural gap the source comment names directly ("writing to
`counter_raw` here overwrites `counter` directly, no logging, no
bounds, no semantics beyond 'poke this memory'") — now demonstrated as
an actual value jump you watched happen, not a hypothetical.

### Step 4 — `info_read`: the debugfs file that *is* real driver code

Not every debugfs file skips this driver's own code — `info` was
registered via `debugfs_create_file("info", 0444, ..., &info_fops)`,
a real `file_operations` struct with a real `read` callback, just like
any other char device:

```gdb
(gdb) delete
(gdb) break info_read
(gdb) continue
```
```bash
# vmb:
cat /sys/kernel/debug/debugfs_sysfs_demo/info
```
```gdb
Thread 2 hit Breakpoint N, info_read (...) at debugfs_sysfs.c:144
(gdb) next   # mutex_lock(&data_lock) - this one DOES take the lock
(gdb) next
(gdb) print c
(gdb) print e
```

Contrast this directly against step 3's `counter_raw`: same
`/sys/kernel/debug/debugfs_sysfs_demo/` directory, but `info` goes
through real, breakable, lock-respecting driver code while
`counter_raw`/`enabled_raw` never touch a line of `debugfs_sysfs.c` at
all — two files sitting side by side in the same debugfs directory,
with fundamentally different amounts of code (and safety) behind them.

## Cleanup

```gdb
(gdb) delete
(gdb) break debugfs_sysfs_exit
(gdb) continue
```
```bash
# vmb:
rmmod debugfs_sysfs
```
```gdb
Thread 2 hit Breakpoint N, debugfs_sysfs_exit () at debugfs_sysfs.c:207
(gdb) next   # debugfs_remove_recursive(demo_debugfs_dir)
```
```bash
# vmb:
poweroff -f
```

## What this proves

"Two interfaces to the same state" is not a metaphor here — a
watchpoint on `enabled` tripping from a write that never entered this
driver's own `enabled_store()` is direct, mechanical proof that sysfs
and debugfs really do share one underlying byte, not two synchronized
copies. And the sysfs mutex genuinely only protects sysfs's own
callers from each other — debugfs's raw bindings bypass it completely,
which is exactly the tradeoff the source comment describes as "that
convenience is also the danger," now backed by a value you watched
jump from 1 to 100 with no lock in the way at all.
