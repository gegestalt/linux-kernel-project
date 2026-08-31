# 04 — module_params

`module_param()` on its own, without the rest of what [03](../03_gpio_sim/)
bundles it with: every parameter type this repo uses elsewhere, plus a demo
device that proves a `0644` parameter is genuinely live-tunable — no reload,
no callback code required.

## What this demonstrates

- `module_param(name, type, perm)` for `charp`, `uint`, and `bool`, and what
  the permission bits actually mean: they're the **sysfs file mode** for
  `/sys/module/<name>/parameters/<name>`, not a general access-control
  setting. `0` → no file at all (load-time only). `0444` → readable, fixed
  after `insmod`. `0644` → root can change it while the module is loaded.
- The fact that a `0644` parameter has **no store callback** by default —
  writing to its sysfs file just overwrites the C variable directly.
  Nothing runs. Code that wants to observe the change has to *re-read the
  variable itself* the next time it needs the value — this module's
  `module_params_read()` does exactly that, which is why `repeat_count` and
  `verbose` change behavior instantly without a reload.
- `module_param_named(sysfs_name, c_variable, type, perm)` — expose a
  variable under a sysfs filename that differs from its identifier in the
  source (`dbg_level` in the code, `log_level` in `/sys/module/.../parameters/`).
- `module_param_array(name, type, &count_var, perm)` — a fixed-size array
  settable at `insmod` time as a comma-separated list, with `count_var`
  telling you how many elements were actually supplied.
- Contrast with lab 03/[16](../16_debugfs_sysfs/): a manually-written
  `DEVICE_ATTR_RW` sysfs attribute needs an explicit `show`/`store` pair you
  write yourself; a `module_param()` gets its sysfs file *for free* — at
  the cost of no validation and no side effects on write.

## Files

| File | Purpose |
|---|---|
| `module_params.c` | The module: five parameters, one misc device whose `read()` recomputes output from current parameter state. |
| `Makefile` | Build, `clean`, `check`/`checkpatch` (same shape as labs 05/07/08). |

## Build

```bash
cd 04_module_params
make
```

## Load and test

```bash
sudo insmod ./module_params.ko
dmesg | tail -10
ls /sys/module/module_params/parameters/
cat /dev/module_params_demo
```

Change a writable parameter with the module already loaded, no reload:

```bash
cat /sys/module/module_params/parameters/repeat_count   # 1
echo 4 | sudo tee /sys/module/module_params/parameters/repeat_count
cat /dev/module_params_demo   # now repeats 4 times — nothing was rebuilt or reloaded

echo 1 | sudo tee /sys/module/module_params/parameters/verbose
cat /dev/module_params_demo   # now includes the diagnostics block too
```

Confirm the read-only parameters really are read-only:

```bash
echo test | sudo tee /sys/module/module_params/parameters/greeting
# tee: /sys/module/module_params/parameters/greeting: Permission denied
echo 5 | sudo tee /sys/module/module_params/parameters/log_level
# same — 0444 means fixed after insmod, root included
```

Load-time array and renamed parameters:

```bash
sudo rmmod module_params
sudo insmod ./module_params.ko primes=11,13,17,19 log_level=7 greeting="custom greeting"
dmesg | tail -6
cat /sys/module/module_params/parameters/primes    # 11,13,17,19
cat /sys/module/module_params/parameters/log_level  # 7 — note the sysfs name differs from the C variable dbg_level
cat /dev/module_params_demo
```

## checkpatch

```bash
make check
```

## Cleanup

```bash
sudo rmmod module_params
dmesg | tail -3
make clean
```

## Things to try

- Pass more than 4 values to `primes=` at load time
  (`primes=2,3,5,7,11`) — `insmod` should refuse to load; read the exact
  error and match it against `module_param_array`'s fixed array size.
- Set `repeat_count` to something absurd, like `999999`, via sysfs, then
  `cat /dev/module_params_demo` — confirm the driver's own
  `MAX_REPEAT` clamp keeps it from doing anything unreasonable. This is the
  same principle lab 03's `poll_ms` range check demonstrates: a `0644`
  parameter (or any userspace-writable knob) is an attacker- or
  mistake-controlled input, and the kernel side must bound it regardless of
  what the sysfs permission bits imply about who's "allowed" to write it.
- Read `include/linux/moduleparam.h` in `../../linux_mainline` and find
  `param_set_uint`/`param_get_uint` — the generic get/set functions
  `module_param(..., uint, ...)` wires up for you. Then look at how
  `module_param_cb()` lets you supply your *own* set/get pair when you do
  want a callback on write (a natural next step once you've felt the limit
  of the free version here).

## Debugging with GDB

For a full, self-contained, step-by-step session for this lab — tmux
pane layout, every command, every output explained — see
[`GDB_WALKTHROUGH.md`](GDB_WALKTHROUGH.md).

Setup: [`../GDB_DEBUGGING.md`](../GDB_DEBUGGING.md). Every target below
confirmed against real debug info first:

```bash
$ gdb -q -batch -nx -ex "file module_params.ko" \
    -ex "info line module_params_init" -ex "info line module_params_exit" \
    -ex "info line module_params_read" -ex "ptype primes" -ex "ptype greeting" \
    module_params.ko
Line 147 of "module_params.c" starts at address 0x888 <module_params_init> and ends at 0x8ac <module_params_init+36>.
Line 177 of "module_params.c" starts at address 0x838 <module_params_exit> and ends at 0x840 <module_params_exit+8>.
Line 96 of "module_params.c" starts at address 0x88 <module_params_read> and ends at 0xac <module_params_read+36>.
type = int [4]
type = char *
```

**`module_params_init` — watch every parameter's *actual* load-time
value, and the primes-formatting loop build its string one write at a
time.** This is the richest stop in this lab: five real parameters to
inspect at once, right where the module first reports them.

```bash
sudo insmod ./module_params.ko primes=11,13,17,19 log_level=7 greeting="custom greeting"
```
```gdb
(gdb) lx-symbols /home/adiopocere/Desktop/codes/linux-kernel-project
(gdb) break module_params_init
(gdb) continue
(gdb) print greeting           # "custom greeting" - the charp parameter, already parsed
(gdb) print repeat_count
(gdb) print dbg_level           # 7 - note the C name differs from the sysfs name (log_level)
(gdb) print primes               # int [4] = {11, 13, 17, 19} - the whole array at once
(gdb) print primes_count
(gdb) next                        # step into the primes_buf formatting loop
(gdb) print primes_len
(gdb) next
(gdb) print primes_buf             # watch the comma-separated string grow, one scnprintf() at a time
(gdb) finish                        # run to return, back out to do_one_initcall
```

Every one of those values is exactly what you passed on the `insmod`
command line, already resolved into the module's own global variables
by the time `module_params_init` runs — direct confirmation that
`module_param()` parsing happens *before* your `init` function is ever
called, not something your code does itself.

**`module_params_read` — the live-tunable read path**, unchanged from
before:

```gdb
(gdb) break module_params_read
(gdb) continue
```
```bash
cat /dev/module_params_demo
```
```gdb
(gdb) print repeat_count      # the live module_param value, no accessor needed
(gdb) print verbose
(gdb) next                     # step through the reps-clamping and the repeat loop itself
```

The point worth confirming live: change `repeat_count` from the guest
shell (`echo 4 | sudo tee /sys/module/module_params/parameters/repeat_count`)
*without* continuing past this breakpoint first, then `continue` and hit
it again — `print repeat_count` shows the new value immediately. There's
no `module_params_repeat_count_store()` function to break on, because
`module_param()` never generates one; the sysfs write lands directly on
the variable, which is exactly what this lab's README explains in prose
and what you're now confirming by watching the raw memory change under
GDB.

**`module_params_exit`** — trivial, but worth the one-line confirmation
that `misc_deregister()` runs before the final `pr_info()`, the same
"clean up the userspace-facing interface first" ordering every
device-backed lab in this repo follows:

```gdb
(gdb) break module_params_exit
(gdb) continue
```
```bash
sudo rmmod module_params
```
```gdb
(gdb) next
(gdb) finish
```

