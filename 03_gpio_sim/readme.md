# 03 — gpio_sim

A full-sized driver over Linux's `gpio-sim` virtual hardware. This is the
first module that isn't a single isolated concept — it's several of them
working together, on purpose: read it as a preview of where modules
[04](../04_module_params/), [09](../09_read_write_cdev/),
[13](../13_kernel_memory/), [14](../14_timers_workqueues/), and
[16](../16_debugfs_sysfs/) are each individually headed, combined into one
driver that watches a simulated button and mirrors its state onto a
simulated LED.

## What this demonstrates

- **GPIO consumer API** — `gpio_device_find_by_label()`,
  `gpio_device_get_desc()`, `gpiod_direction_input()`/`_output()`,
  `gpiod_get_value_cansleep()`/`gpiod_set_value_cansleep()`. The `_cansleep`
  suffix matters: these calls may block (the backing GPIO controller could
  be behind a sleeping bus like I2C/SPI on real hardware), so they're only
  safe from process context — never from an interrupt handler or other
  atomic context.
- **Module parameters** (`module_param()`, read-only at 0444) — which GPIO
  chip and offsets to bind to, and the initial polling interval. See module
  [04](../04_module_params/) for parameters in isolation.
- **Dynamic kernel memory** — driver state lives in a `kzalloc()`'d struct,
  not a global, so its lifetime and layout are explicit. See module
  [13](../13_kernel_memory/).
- **A mutex-protected shared state struct** — the same `struct
  gpioctrl_state` is read and written from the workqueue callback, sysfs
  show/store callbacks, and the `/dev/gpioctrl` read/write callbacks, all of
  which can run concurrently. See module [11](../11_concurrency_locking/).
- **Delayed work** (`struct delayed_work`, `schedule_delayed_work()`,
  `mod_delayed_work()`, `cancel_delayed_work_sync()`) polling the button on
  a timer and rescheduling itself. See module [14](../14_timers_workqueues/).
- **sysfs attributes** (`DEVICE_ATTR_RO`/`DEVICE_ATTR_RW`,
  `sysfs_create_group()`) for both read-only telemetry and read/write
  controls. See module [16](../16_debugfs_sysfs/).
- **A misc character device** (`misc_register()`) as the "everything at
  once" interface: `cat /dev/gpioctrl` dumps full state, `echo cmd >
  /dev/gpioctrl` drives it with a tiny text command language.
- **Execution-context bookkeeping** — every state change records
  `current->pid`/`current->comm`, so you can see, live, which task
  (`insmod`, a `kworker`, or your own shell via `echo`) touched the driver.

## Files

| File | Purpose |
|---|---|
| `gpioctrl.c` | The driver. |
| `Makefile` | Out-of-tree Kbuild wrapper. |

## Build

```bash
cd 03_gpio_sim
make
```

## Set up the simulated hardware first

`gpio-sim` is a real in-kernel module that creates GPIO chips entirely in
software, configured through configfs — no physical wiring required.

```bash
sudo modprobe gpio-sim

# Create one simulated GPIO chip ("gpio-device") with one bank of lines.
# The bank's directory name becomes part of its default label
# ("<dev_name>:<bank-dir-name>"), which is why this module's default
# gpio_label parameter is "gpio-sim.0:node0" — it's naming the bank "node0".
sudo mkdir -p /sys/kernel/config/gpio-sim/gpio-device/node0
echo 22 | sudo tee /sys/kernel/config/gpio-sim/gpio-device/node0/num_lines
# button_offset=20 and led_offset=21 are the defaults, so we need at least
# 22 lines (0-21).

# Instantiate the device.
echo 1 | sudo tee /sys/kernel/config/gpio-sim/gpio-device/live

# Confirm the actual platform device name — it will be "gpio-sim.0" unless
# another gpio-sim device already exists on this machine, in which case
# adjust the gpio_label module parameter below to match.
cat /sys/kernel/config/gpio-sim/gpio-device/dev_name

# Find which /dev/gpiochipN this became, for reference:
gpiodetect | grep gpio-sim
```

## Load and test

```bash
sudo insmod ./gpioctrl.ko
dmesg | tail -20     # init log: state pointer, size, chip, offsets, timing

# Full state dump:
cat /dev/gpioctrl

# Individual sysfs attributes:
ls /sys/class/misc/gpioctrl/
cat /sys/class/misc/gpioctrl/button
cat /sys/class/misc/gpioctrl/led
cat /sys/class/misc/gpioctrl/samples
```

Find the simulated line's own sysfs group (created by `gpio-sim` itself, one
level below the gpiochip found above) to drive the button from outside the
driver:

```bash
GPIOCHIP=$(gpiodetect | grep gpio-sim | awk '{print $1}')
BUTTON_SYSFS=/sys/devices/platform/gpio-sim.0/$GPIOCHIP/sim_gpio20

# Simulate pressing the button (pull it up):
echo pull-up | sudo tee $BUTTON_SYSFS/pull
sleep 1
cat /sys/class/misc/gpioctrl/led      # should now read 1
dmesg | tail -3                        # workqueue poll picked up the change

# Release it:
echo pull-down | sudo tee $BUTTON_SYSFS/pull
sleep 1
cat /sys/class/misc/gpioctrl/led      # back to 0
```

Drive the controls both ways — sysfs and the `/dev` command language should
behave identically:

```bash
echo 1 | sudo tee /sys/class/misc/gpioctrl/invert     # LED now mirrors button inverted
echo "invert=0" | sudo tee /dev/gpioctrl                # same effect, via /dev

echo 100 | sudo tee /sys/class/misc/gpioctrl/poll_ms   # poll faster
echo "poll_ms=500" | sudo tee /dev/gpioctrl             # back to slower, via /dev

echo sync | sudo tee /dev/gpioctrl          # force an immediate sample outside the timer
echo reset_stats | sudo tee /dev/gpioctrl   # zero the counters, watch `samples`/`changes` reset

echo "poll_ms=5" | sudo tee /dev/gpioctrl   # rejected: -ERANGE, below MIN_POLL_MS
```

Watch `dmesg -w` in a separate shell throughout — every control write and
every detected button change logs the acting task's `comm[pid]`, so you can
see `bash[...]` for your `echo` commands and `kworker/...` for the periodic
poll.

## Cleanup

```bash
sudo rmmod gpioctrl
dmesg | tail -10    # exit log: final state, uptime, counters

# Tear down the simulated hardware:
echo 0 | sudo tee /sys/kernel/config/gpio-sim/gpio-device/live
sudo rmdir /sys/kernel/config/gpio-sim/gpio-device/node0
sudo rmdir /sys/kernel/config/gpio-sim/gpio-device
sudo modprobe -r gpio-sim

make clean
```

## Things to try

- Load with non-default parameters: `sudo insmod ./gpioctrl.ko
  button_offset=5 led_offset=6` against a bank with enough lines — confirm
  it binds to different offsets and that `/sys/module/gpioctrl/parameters/`
  shows the values you passed (and that writing to those files fails: they're
  mode 0444).
- `rmmod` the module while a `sim_gpio20/pull` write is in flight, or while
  the poll interval is very short (`poll_ms=10`) — this exercises
  `cancel_delayed_work_sync()`'s job of guaranteeing no worker touches
  driver state after `kfree(state)` runs. It should always be clean; if you
  ever see a crash here, that's the bug class module 11 is about.
- Try to load two instances against the same `gpio_label` with two
  different device nodes — `misc_register()` will fail the second time
  because `DRIVER_NAME` is a fixed name; read the resulting `dmesg` error
  path (`err_misc`).

## Debugging with GDB

For a full, self-contained, step-by-step session for this module — tmux
pane layout, every command, every output explained — see
[`gdb_walkthrough.md`](gdb_walkthrough.md).

Setup: [`../gdb_debugging.md`](../gdb_debugging.md). This is the module
where stepping through GDB pays off most — you can watch the button read
turn into the LED write, one line at a time, including stepping *into*
the GPIO subsystem's own backend code:

```gdb
(gdb) lx-symbols /home/adiopocere/Desktop/codes/linux-kernel-project
(gdb) break gpioctrl_sample_once
(gdb) continue
```
```bash
GPIOCHIP=$(gpiodetect | grep gpio-sim | awk '{print $1}')
echo pull-up | sudo tee /sys/devices/platform/gpio-sim.0/$GPIOCHIP/sim_gpio20/pull
```
```gdb
(gdb) next                       # past gpiod_get_value_cansleep(button)
(gdb) print button_value
(gdb) next                        # past the invert ternary
(gdb) print desired_led
(gdb) step                         # STEP INTO gpiod_set_value_cansleep() itself
(gdb) bt                            # now inside gpio-sim.c's own set-value backend
(gdb) print *state                  # the whole driver state struct, live
```

All the breakpoint targets below were confirmed against real compiled
debug info before being written down:

```bash
$ gdb -q -batch -nx -ex "file gpioctrl.ko" \
    -ex "info line gpioctrl_init" -ex "info line gpioctrl_exit" \
    -ex "info line gpioctrl_write" -ex "info line gpioctrl_work_fn" \
    -ex "ptype struct gpioctrl_state" gpioctrl.ko
Line 1056 of "gpioctrl.c" starts at address 0x1dd0 <gpioctrl_init> ...
Line 175 of ".../timekeeping.h" starts at address 0x1c58 <gpioctrl_exit> ...
Line 856 of "gpioctrl.c" starts at address 0xce8 <gpioctrl_write> ...
Line 324 of "gpioctrl.c" starts at address 0xc10 <gpioctrl_work_fn> ...
type = struct gpioctrl_state {
    struct mutex lock;
    int button;
    int led;
    bool invert;
    unsigned int poll_ms;
    u64 samples;
    u64 changes;
    u64 output_updates;
    u64 loaded_at_ns;
    pid_t init_pid;
    char init_comm[16];
    pid_t last_sample_pid;
    char last_sample_comm[16];
}
```

That `gpioctrl_exit` line is a genuine, worth-knowing DWARF quirk, not a
mistake: the *very first* statement inside `gpioctrl_exit()` is
`start_ns = ktime_get_ns();`, and `ktime_get_ns()` is a `static inline`
function defined in the kernel's own `timekeeping.h` — so the function's
entry address maps to that inlined callee's source line, not
`gpioctrl_exit`'s own opening brace. `info address gpioctrl_exit` still
correctly reports a real function address, and `break gpioctrl_exit`
still sets a perfectly good breakpoint there — GDB is just being
literal about which source line owns the very first machine
instruction, and that line happens to belong to an inlined header
function.

**`gpioctrl_init` — the resource-acquisition sequence, including its
error paths.** This module's `init` does more real work than any other
in the repo: `kzalloc`, GPIO chip/descriptor lookup, direction
configuration, `misc_register`, `sysfs_create_group`, then
`schedule_delayed_work` — five things that can each fail, each with its
own `goto` target.

```bash
sudo modprobe gpio-sim
sudo mkdir -p /sys/kernel/config/gpio-sim/gpio-device/node0
echo 22 | sudo tee /sys/kernel/config/gpio-sim/gpio-device/node0/num_lines
echo 1  | sudo tee /sys/kernel/config/gpio-sim/gpio-device/live
```
```gdb
(gdb) break do_init_module
(gdb) continue
```
```bash
sudo insmod ./gpioctrl.ko
```
```gdb
(gdb) lx-symbols /home/adiopocere/Desktop/codes/linux-kernel-project
(gdb) break gpioctrl_init
(gdb) continue
(gdb) next                     # past kzalloc() - state now points at real, zeroed memory
(gdb) print state
(gdb) print *state              # everything zero except what mutex_init() touches next
(gdb) next                       # mutex_init(), then the button/led/poll_ms/invert defaults
(gdb) next
(gdb) print state->poll_ms       # matches initial_poll_ms (module param, default 500)
(gdb) next                        # past gpio_device_find_by_label()
(gdb) print gdev                   # non-NULL if gpio-sim's device was found by that exact label
(gdb) next                          # past gpio_device_get_desc() x2 - button and led descriptors
(gdb) print button
(gdb) print led
(gdb) finish                         # run to return - watch the return value: 0 means every step succeeded
```

Rerun with a *wrong* `gpio_label` (`sudo insmod ./gpioctrl.ko
gpio_label=gpio-sim.0:doesnotexist`) and step through the same sequence
— `gpio_device_find_by_label()` returns `NULL`, `print gdev` shows it,
and `next` walks straight to the `err_free_state:` label instead of
continuing down the happy path. Watching the *actual* branch taken beats
reading the `goto` labels in the source.

**`gpioctrl_write` — the `/dev/gpioctrl` command parser**, one
breakpoint, four different real inputs:

```gdb
(gdb) break gpioctrl_write
(gdb) continue
```
```bash
echo "invert=1"    | sudo tee /dev/gpioctrl
```
```gdb
(gdb) print count            # the exact byte count "invert=1\n" produced
(gdb) next                    # step through copy_from_user(), kbuf[count]='\0', strim()
(gdb) print cmd                # "invert=1" - null-terminated, trimmed
(gdb) next                      # into the sysfs_streq()/strncmp() chain deciding which branch
(gdb) continue                   # let it finish, hit the breakpoint again on the NEXT write
```
```bash
echo "poll_ms=abc" | sudo tee /dev/gpioctrl    # deliberately malformed
```
```gdb
(gdb) print cmd               # "poll_ms=abc"
(gdb) next                     # kstrtouint() on "abc" - watch `ret` come back non-zero
(gdb) print ret
(gdb) finish                    # returns ret itself (a negative errno), not count - confirm at the shell:
```
```bash
echo "poll_ms=abc" | sudo tee /dev/gpioctrl
# tee: /dev/gpioctrl: Invalid argument
```

**`gpioctrl_work_fn` — catch the periodic poll running from a
`kworker`**, not from any shell command at all:

```gdb
(gdb) break gpioctrl_work_fn
(gdb) continue
```

No trigger needed — it fires on its own every `poll_ms` (500ms by
default). When it hits: `print current->comm` and `print current->pid`
(a real `kworker/N:M`, not your shell), then `next` through
`gpioctrl_sample_once()`'s call and the `mod_delayed_work()`/
`schedule_delayed_work()` reschedule at the end — the exact mechanism
that keeps this callback running forever until `gpioctrl_exit()` calls
`cancel_delayed_work_sync()`.

**`gpioctrl_exit` — the cleanup ordering**, and why it's ordered the
way it is:

```gdb
(gdb) break gpioctrl_exit
(gdb) continue
```
```bash
sudo rmmod gpioctrl
```
```gdb
(gdb) next    # cancel_delayed_work_sync() FIRST - no poll can touch state after this line
(gdb) next     # THEN sysfs_remove_group() - userspace can't reach state's sysfs files anymore
(gdb) next      # THEN misc_deregister() - /dev/gpioctrl is gone
(gdb) print state->samples   # still readable here - state itself isn't freed until kfree() near the end
(gdb) finish
```

Deliberately reorder those three calls in the source (`kfree(state)`
before `cancel_delayed_work_sync()`, say) and reason through what a
still-running `kworker` calling into `gpioctrl_work_fn` would dereference
— this is the exact use-after-free shape `cancel_delayed_work_sync()`
being called *first* prevents. `watch state->samples` (no function
breakpoint needed) is the cleanest way to catch the poll counter
incrementing from a completely different trigger — a real button press
via `sim_gpio20/pull` — without stopping anywhere else first.

