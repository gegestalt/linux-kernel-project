# 03 — gpio_sim

A full-sized driver over Linux's `gpio-sim` virtual hardware. This is the
first lab that isn't a single isolated concept — it's several of them
working together, on purpose: read it as a preview of where labs
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
  chip and offsets to bind to, and the initial polling interval. See lab
  [04](../04_module_params/) for parameters in isolation.
- **Dynamic kernel memory** — driver state lives in a `kzalloc()`'d struct,
  not a global, so its lifetime and layout are explicit. See lab
  [13](../13_kernel_memory/).
- **A mutex-protected shared state struct** — the same `struct
  gpioctrl_state` is read and written from the workqueue callback, sysfs
  show/store callbacks, and the `/dev/gpioctrl` read/write callbacks, all of
  which can run concurrently. See lab [11](../11_concurrency_locking/).
- **Delayed work** (`struct delayed_work`, `schedule_delayed_work()`,
  `mod_delayed_work()`, `cancel_delayed_work_sync()`) polling the button on
  a timer and rescheduling itself. See lab [14](../14_timers_workqueues/).
- **sysfs attributes** (`DEVICE_ATTR_RO`/`DEVICE_ATTR_RW`,
  `sysfs_create_group()`) for both read-only telemetry and read/write
  controls. See lab [16](../16_debugfs_sysfs/).
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
  ever see a crash here, that's the bug class lab 11 is about.
- Try to load two instances against the same `gpio_label` with two
  different device nodes — `misc_register()` will fail the second time
  because `DRIVER_NAME` is a fixed name; read the resulting `dmesg` error
  path (`err_misc`).

## Debugging with GDB

Setup: [`../GDB_DEBUGGING.md`](../GDB_DEBUGGING.md). This is the lab
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

Two more useful breakpoints for this specific module: `gpioctrl_write`
(to watch the `invert=`/`poll_ms=`/`sync`/`reset_stats` command parser
decide which branch to take) and `gpioctrl_work_fn` (to catch the
periodic poll running from a `kworker` — `lx-ps` right after hitting it
will show you which one). `watch state->samples` is a clean way to stop
the instant the poll counter increments, without needing a function
breakpoint at all.

## Tracing this live

Setup and general method: [`../FTRACE_TRACING.md`](../FTRACE_TRACING.md).
Start from discovery, not from being told a function name — with the
module already loaded:

```bash
sudo bpftrace -l 'kprobe:gpioctrl:*'
```
```
kprobe:gpioctrl:gpioctrl_open
kprobe:gpioctrl:gpioctrl_read
kprobe:gpioctrl:gpioctrl_release
kprobe:gpioctrl:gpioctrl_sample_once
kprobe:gpioctrl:gpioctrl_work_fn
kprobe:gpioctrl:gpioctrl_write
```

That's the whole menu, straight from the kernel's symbol table. Probe
the one that runs on every poll, and — since this driver's whole point
is calling into a *different* module's backend — probe that module's
functions too:

```bash
sudo bpftrace -e '
kprobe:gpioctrl_sample_once { printf("-> %s called by %s[%d]\n", probe, comm, pid); }
kprobe:gpio_sim_get         { printf("     descends into %s\n", probe); }
kprobe:gpio_sim_set         { printf("     descends into %s\n", probe); }
'
```

Real captured output, triggered by an actual button press
(`echo pull-up > .../sim_gpio20/pull`):

```
Attached 3 probes
-> kprobe:gpioctrl_sample_once called by kworker/3:0[88823]
     descends into kprobe:gpio_sim_get
-> kprobe:gpioctrl_sample_once called by kworker/3:0[88823]
     descends into kprobe:gpio_sim_get
     descends into kprobe:gpio_sim_set
-> kprobe:gpioctrl_sample_once called by kworker/3:0[88823]
     descends into kprobe:gpio_sim_get
```

Live and unscripted: most poll cycles only read (`gpio_sim_get`,
nothing changed); the middle one also writes (`gpio_sim_set` — the exact
cycle the button press landed in). `[gpio_sim]`/`gpio_sim_*` really is a
different `.ko`'s own code, reached directly from `gpioctrl`'s.
`cat /sys/class/misc/gpioctrl/led` afterward confirms it actually
changed to `1`.

