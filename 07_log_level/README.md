# 07 — log_level

Every `printk()` priority, side by side, so you can see exactly what
controls whether a kernel message reaches your console versus only the
kernel's internal ring buffer.

## What this demonstrates

- The eight `printk()` priorities, from most to least severe:
  `KERN_EMERG`(0) → `KERN_ALERT`(1) → `KERN_CRIT`(2) → `KERN_ERR`(3) →
  `KERN_WARNING`(4) → `KERN_NOTICE`(5) → `KERN_INFO`(6) → `KERN_DEBUG`(7),
  and the `pr_emerg()`/`pr_alert()`/`pr_crit()`/`pr_err()`/`pr_warn()`/
  `pr_notice()`/`pr_info()`/`pr_debug()` helpers that already embed the
  matching `KERN_*` prefix, so you don't write it yourself.
- `#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt` — the standard way to get a
  consistent `modname: ` prefix on every `pr_*()` call in a file without
  repeating it at every call site. Compare this module's log lines against
  `01`/`02`'s bare `printk(KERN_INFO "...")`, which has no such prefix.
- `pr_debug()`'s special case: unlike the other seven, whether it prints
  *at all* depends on kernel configuration (`CONFIG_DYNAMIC_DEBUG` /
  `CONFIG_DYNAMIC_DEBUG_CORE`, or a plain build-time `#define DEBUG`) —
  it's not just "the lowest priority," it can be compiled out or gated
  per-callsite.
- The **console loglevel**: a message is only ever printed to the console
  (as opposed to just landing in the ring buffer, retrievable with `dmesg`)
  if its priority number is *lower* (more severe) than the current console
  loglevel. This module's output makes that threshold directly visible on
  screen.

## Files

| File | Purpose |
|---|---|
| `printk_log_levels.c` | The module. |
| `Makefile` | Build, `clean`, and `check`/`checkpatch` targets (mirrors lab 05, with `--ignore=PREFER_PR_LEVEL` since this lab's whole point is comparing raw `printk(KERN_DEBUG ...)` against `pr_debug()`). |

## Build

```bash
cd 07_log_level
make
```

## Load and test

```bash
# See the current console loglevel (four numbers: current, default,
# minimum, boot-time-default):
cat /proc/sys/kernel/printk

sudo insmod ./printk_log_levels.ko
dmesg | tail -12    # all nine lines land in the ring buffer regardless of console loglevel
sudo rmmod printk_log_levels
```

Now change the console loglevel and reload, watching your actual terminal
(not `dmesg`) for what prints live. This needs a real console, so run it
somewhere you can watch kernel messages appear on screen directly — e.g. a
plain VT (`Ctrl+Alt+F2`) or `journalctl -kf` in a second terminal, since
many desktop/SSH sessions don't show kernel console output directly.

```bash
# Only emerg/alert/crit (0-2) reach the console:
echo 3 > /proc/sys/kernel/printk
sudo insmod ./printk_log_levels.ko && sudo rmmod printk_log_levels
# On the console: nothing (this module never uses KERN_EMERG/ALERT/CRIT).
# In `dmesg`: all nine lines, as always — dmesg reads the ring buffer,
# which every priority reaches; only the *console* is filtered.

# Open it back up to include info-level:
echo 7 > /proc/sys/kernel/printk
sudo insmod ./printk_log_levels.ko && sudo rmmod printk_log_levels
# Now everything except pr_debug (and possibly that too, depending on
# CONFIG_DYNAMIC_DEBUG) shows up live.
```

Compare the two KERN_DEBUG lines directly — one via `pr_debug()`, one via a
raw `printk(KERN_DEBUG ...)`:

```bash
dmesg | grep 'level 7'
```

If `CONFIG_DYNAMIC_DEBUG` is enabled on this kernel, you can also flip
`pr_debug()` on/off for just this module at runtime without touching the
console loglevel at all:

```bash
# Check whether dynamic debug is available:
ls /sys/kernel/debug/dynamic_debug/control 2>/dev/null && echo "available"

# If it is (needs debugfs mounted, usually at /sys/kernel/debug):
sudo insmod ./printk_log_levels.ko
grep printk_log_levels /sys/kernel/debug/dynamic_debug/control
echo 'module printk_log_levels +p' | sudo tee /sys/kernel/debug/dynamic_debug/control
sudo rmmod printk_log_levels && sudo insmod ./printk_log_levels.ko
dmesg | tail -3   # pr_debug() line now present even with a high console loglevel
sudo rmmod printk_log_levels
```

## checkpatch

```bash
make check
```

## Cleanup

```bash
# Restore whatever console loglevel this system normally runs with, if you changed it:
echo 4 > /proc/sys/kernel/printk   # 4 is Debian/Ubuntu's typical default; check yours first
make clean
```

## Things to try

- Set the console loglevel to exactly `7` (include debug) versus `8`
  (include everything, though 7 priorities is all there is) — confirm they
  behave identically, since `KERN_DEBUG` is priority 7 and there's nothing
  numerically lower.
- Look up `KERN_SOH` / `KERN_SOH_ASCII` and `pr_fmt` (or better, the
  `printk` LOGLEVEL encoding: `\001` + digit) to understand *how* the
  priority is actually embedded in the message string the kernel receives
  — it's not a separate syscall argument, it's a prefix on the format
  string itself.
- Time `dmesg`'s ring buffer against a flood of `pr_debug()` calls in a
  loop (careful — this can wrap the ring buffer and evict older messages,
  including messages from *other* subsystems). A tame version: load/unload
  this module in a loop 50 times and see how much of the earlier output
  survives in `dmesg`.

## Debugging with GDB

Setup: [`../GDB_DEBUGGING.md`](../GDB_DEBUGGING.md). This lab's own
premise — "`dmesg -w` isn't the whole picture" — is exactly what
`lx-dmesg` fixes: it reads the ring buffer straight out of kernel
memory, so it works even with the guest frozen mid-breakpoint.

```gdb
(gdb) lx-symbols /home/adiopocere/Desktop/codes/linux-kernel-project
(gdb) break printk_emit_all_levels
(gdb) continue
```
```bash
sudo insmod ./printk_log_levels.ko
```
```gdb
(gdb) next                  # step across each pr_emerg()/pr_alert()/.../pr_debug() call in turn
(gdb) lx-dmesg               # see each line land in the ring buffer, live, one at a time
```

Stepping one `pr_*()` call at a time and re-running `lx-dmesg` after each
`next` is the clearest possible way to see that every priority reaches
the ring buffer unconditionally — the console-loglevel filtering this
lab's README discusses only ever affects the *live console*, never what
`lx-dmesg`/`dmesg` can retrieve afterward.

