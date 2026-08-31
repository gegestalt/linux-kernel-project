# 16 — debugfs_sysfs

The exact same two pieces of state (`counter`, `enabled`), exposed twice:
once through hand-written sysfs attributes (the pattern every earlier lab
in this repo uses), once through debugfs's variable-binding helpers. Same
data, radically different amount of code and radically different
guarantees.

## What this demonstrates

- **sysfs is part of the kernel's userspace ABI.** Once an attribute
  ships under `/sys/...`, breaking it is treated the same as breaking any
  other syscall-level promise (see `Documentation/ABI/` in
  `../../linux_mainline`). That's why every sysfs attribute in this repo,
  including this lab's, is a hand-written `show()`/`store()` pair: the
  validation and side effects are the point, not incidental boilerplate.
- **debugfs is explicitly, documentedly *not* an ABI.**
  `Documentation/filesystems/debugfs.rst` says so directly — files can
  appear, move, or vanish across kernel versions with no deprecation
  period, and nothing in userspace should ever hard-depend on a debugfs
  path existing.
- **What that non-guarantee buys you:** `debugfs_create_u32()` and
  `debugfs_create_bool()` bind a debugfs file directly to a variable's
  address — one line, no `show()`, no `store()`, no validation. Compare
  `counter`'s sysfs attribute (a `show()` function, a `struct
  kobj_attribute`, an entry in the attribute array — about a dozen lines)
  against `counter_raw`'s debugfs entry (one line, same underlying `u32`).
- **...and what it costs:** `counter_raw` accepts *any* `u32` you write to
  it — no logging, no bounds, no semantics beyond "overwrite this memory."
  The sysfs side's `increment` attribute, by contrast, only allows the one
  operation this driver actually intends to support, and logs who did it.
  This is the real tradeoff: debugfs is for a developer poking at internal
  state while debugging, not a designed interface for the values it
  happens to expose.
- **Never gate driver function on debugfs succeeding.** `debugfs_create_dir()`/
  `_u32()`/`_bool()`/`_file()` are called here without checking their
  return values at all — if `CONFIG_DEBUG_FS` is off, or debugfs isn't
  mounted, these become harmless no-ops (the debugfs helpers all tolerate
  being handed a `NULL`/`ERR_PTR` parent). A driver whose actual
  functionality depends on a debug/inspection interface existing has that
  exactly backwards.

## Files

| File | Purpose |
|---|---|
| `debugfs_sysfs.c` | The module: sysfs (`counter`/`enabled`/`increment`) and debugfs (`counter_raw`/`enabled_raw`/`info`) exposing the same underlying state. |
| `Makefile` | Build, `clean`, `check`/`checkpatch`. |

## Build

```bash
cd 16_debugfs_sysfs
make
```

## Load and test

```bash
sudo insmod ./debugfs_sysfs.ko
dmesg | tail -3

ls /sys/kernel/debugfs_sysfs_demo/
ls /sys/kernel/debug/debugfs_sysfs_demo/     # needs root, or CONFIG_DEBUG_FS_ALLOW_ALL
```

Compare the two counters — they're the same variable:

```bash
cat /sys/kernel/debugfs_sysfs_demo/counter
sudo cat /sys/kernel/debug/debugfs_sysfs_demo/counter_raw
```

The controlled sysfs path — validated, logged:

```bash
echo 1 | sudo tee /sys/kernel/debugfs_sysfs_demo/increment
dmesg | tail -2    # "sysfs: increment -> counter=1 by ..."
cat /sys/kernel/debugfs_sysfs_demo/counter    # 1
```

The raw debugfs path — direct memory poke, no logging, no rules:

```bash
echo 9999 | sudo tee /sys/kernel/debug/debugfs_sysfs_demo/counter_raw
dmesg | tail -2                                            # nothing logged
cat /sys/kernel/debugfs_sysfs_demo/counter                 # 9999 — same variable
```

`enabled` behaves the same way on both sides — try flipping it through
`debugfs`'s `enabled_raw` and watch it show up on the sysfs side instantly,
with no `dmesg` line either:

```bash
echo 0 | sudo tee /sys/kernel/debug/debugfs_sysfs_demo/enabled_raw
cat /sys/kernel/debugfs_sysfs_demo/enabled     # 0
```

The debugfs `info` file (custom fops, not the free variable-binding
helpers) for a combined read:

```bash
sudo cat /sys/kernel/debug/debugfs_sysfs_demo/info
```

## checkpatch

```bash
make check
```

## Cleanup

```bash
sudo rmmod debugfs_sysfs
dmesg | tail -3
ls /sys/kernel/debug/debugfs_sysfs_demo   # gone - debugfs_remove_recursive() tore it down
make clean
```

## Things to try

- Count the actual lines of code behind `counter` (sysfs) versus
  `counter_raw` (debugfs) in `debugfs_sysfs.c` — this is the whole
  argument for debugfs made concrete, not asserted.
- `rmmod` the module, then `sudo modprobe -r debugfs 2>/dev/null; sudo
  umount /sys/kernel/debug` (if you're willing to lose debugfs for the
  rest of this session), reload the module, and confirm it still loads
  and its *sysfs* interface still works fully — only the debugfs paths
  are affected, exactly per the "never gate function on debugfs" rule
  above. Remount with `sudo mount -t debugfs debugfs /sys/kernel/debug`
  afterward.
- Read `Documentation/ABI/testing/` in `../../linux_mainline` for real
  examples of sysfs attributes with formal stability documentation, then
  `Documentation/filesystems/debugfs.rst` for debugfs's explicit
  "there is no stability guarantee" language — the contrast is the point
  of this whole lab, stated in the kernel's own words rather than this
  README's.
