# Runbook: building, running, and reading every lab

This is the linear companion to the per-lab `README.md` files: one pass
through labs 01–17, in order, with the exact commands and — the part that
matters most — what you should actually see happen at each step, so you
know whether it worked or you're looking at a bug. Each lab's own
`README.md` has the full explanation and more exercises; this document is
for sitting next to a terminal while you work through the sequence.

Run all of this inside a disposable VM, not bare metal — see the
top-level [`README.md`](README.md#a-word-on-safety) for why, and lab 14's
entry below in particular is only safe there.

## Before you start

```bash
uname -r
ls /lib/modules/$(uname -r)/build   # must exist (install linux-headers-$(uname -r) if not)
git -C linux_mainline log -1 --oneline   # confirms the checkpatch source tree is present
which gpiodetect gpiofind             # from libgpiod tools, needed for lab 03
lsmod | grep gpio_sim || sudo modprobe gpio-sim   # confirm the gpio-sim module loads
grep CONFIG_DEBUG_FS /boot/config-$(uname -r)      # needed for lab 16's debugfs half
grep CONFIG_DEBUG_ATOMIC_SLEEP /boot/config-$(uname -r)  # informational, for lab 14
```

Open a second terminal now and leave it on `dmesg -w` (or `sudo journalctl
-kf`) for the entire session — nearly every "what to look for" below is a
kernel log line, and it's easier to watch it scroll live than to keep
re-running `dmesg | tail`.

A generic per-lab cycle, if you'd rather skip ahead to a specific one:

```bash
cd NN_lab_name && make && sudo insmod ./*.ko
# ... interact ...
sudo rmmod <module_name> && make clean
```

---

## 01 — hello_init

```bash
cd 01_hello_init && make
sudo insmod ./hello.ko
```
**Look for:** `dmesg | tail -3` shows `Hello luv .` — that's the entire
module.
```bash
sudo rmmod hello
```
**Look for:** `bye bye my luv`.

## 02 — better_hello

```bash
cd 02_better_hello && make
sudo insmod ./better_hello.ko && dmesg | tail -3
modinfo ./better_hello.ko
```
**Look for:** the same greeting as lab 01, but now `modinfo` actually
shows `author:` and `description:` fields — lab 01's didn't.
```bash
sudo rmmod better_hello
```

## 03 — gpio_sim

Needs setup first — a simulated GPIO chip via configfs (full detail in
the lab's own README):
```bash
sudo modprobe gpio-sim
sudo mkdir -p /sys/kernel/config/gpio-sim/gpio-device/node0
echo 22 | sudo tee /sys/kernel/config/gpio-sim/gpio-device/node0/num_lines
echo 1 | sudo tee /sys/kernel/config/gpio-sim/gpio-device/live
```
```bash
cd 03_gpio_sim && make
sudo insmod ./gpioctrl.ko
GPIOCHIP=$(gpiodetect | grep gpio-sim | awk '{print $1}')
echo pull-up | sudo tee /sys/devices/platform/gpio-sim.0/$GPIOCHIP/sim_gpio20/pull
```
**Look for:** `cat /sys/class/misc/gpioctrl/led` flips to `1` within one
polling interval (default 500ms), and `dmesg` logs the button-change with
the acting task (`kworker/...` for the periodic poll).
```bash
sudo rmmod gpioctrl
echo 0 | sudo tee /sys/kernel/config/gpio-sim/gpio-device/live
sudo rmdir /sys/kernel/config/gpio-sim/gpio-device/node0 /sys/kernel/config/gpio-sim/gpio-device
```

## 04 — module_params

```bash
cd 04_module_params && make
sudo insmod ./module_params.ko
cat /dev/module_params_demo
echo 4 | sudo tee /sys/module/module_params/parameters/repeat_count
cat /dev/module_params_demo
```
**Look for:** the second `cat` repeats the greeting 4 times instead of 1
— with **no reload in between**. That's the whole point: a `0644`
`module_param()` takes effect immediately because there's no store
callback to wire up.
```bash
sudo rmmod module_params
```

## 05 — register_cdev

```bash
cd 05_register_cdev && make
sudo insmod ./register_cdev.ko
dmesg | tail -3   # note the allocated major number
MAJOR=$(dmesg | grep -oP 'register_cdev: init: major=\K[0-9]+' | tail -1)
sudo mknod /dev/register_cdev c "$MAJOR" 0
cat /dev/register_cdev
```
**Look for:** real multi-line text (major/minor/context) — if you see a
literal `\n` instead of line breaks here, you're on an unpatched
checkout; see PR fixing this bug and update. `dmesg` shows matched
`open:`/`read:`/`release:` lines for the `cat`.
```bash
sudo rm -f /dev/register_cdev && sudo rmmod register_cdev
```

## 06 — procfs_seqfile

```bash
cd 06_procfs_seqfile && make
sudo insmod ./procfs_seqfile.ko
cat /proc/procfs_demo/events
cat /proc/procfs_demo/events
cat /proc/procfs_demo/info
```
**Look for:** the second `events` read shows **two** lines, not one —
each open of that file logs itself as an event. `info` shows
`total_opens` having counted both.
```bash
sudo rmmod procfs_seqfile
```

## 07 — log_level

```bash
cd 07_log_level && make
sudo insmod ./printk_log_levels.ko
dmesg | tail -12
sudo rmmod printk_log_levels
```
**Look for:** nine log lines covering every `printk` priority
(`KERN_EMERG` through `KERN_DEBUG`, plus a raw-`printk` vs. `pr_debug()`
comparison at the debug level). All nine land in `dmesg` regardless of
console loglevel — only a *live console* (not `dmesg`) is filtered by
`/proc/sys/kernel/printk`; see the lab's README for that experiment.

## 08 — open_release_cdev

```bash
cd 08_open_release_cdev && make
sudo insmod ./open_release_cdev.ko
MAJOR=$(dmesg | grep -oP "registered 'open_release_cdev' major=\K[0-9]+" | tail -1)
sudo mknod /dev/open_release_cdev0 c "$MAJOR" 0
./cdev_test /dev/open_release_cdev0
```
**Look for:** four immediate OPEN/RELEASE pairs, then — for the `dup()`
test — **one** OPEN followed by a **~1 second delayed, single** RELEASE,
proving that closing one of two duplicated file descriptors does not
release the underlying open file description; only closing both does.
```bash
sudo rm -f /dev/open_release_cdev0 && sudo rmmod open_release_cdev
```

## 09 — read_write_cdev

```bash
cd 09_read_write_cdev && make
sudo insmod ./read_write_cdev.ko
ls -l /dev/read_write_cdev0   # exists immediately, no mknod needed
echo -n "hello kernel" | sudo tee /dev/read_write_cdev0 > /dev/null
sudo cat /dev/read_write_cdev0
```
**Look for:** `hello kernel` echoed back exactly. Then push more than
4096 bytes at it and confirm you get a **short write** (fewer bytes
written than requested), not an error — see the README's Python snippet.
```bash
sudo rmmod read_write_cdev
```

## 10 — ioctl_basics

```bash
cd 10_ioctl_basics && make
sudo insmod ./ioctl_basics.ko
sudo ./ioctl_test /dev/ioctl_basics0
```
**Look for:** identity/upper/reverse echo transforms in sequence, then an
invalid `SET_MODE` failing with `EINVAL`, then a made-up ioctl command
number failing with `ENOTTY` (Inappropriate ioctl for device) — the two
different, deliberately-distinguished error paths.
```bash
sudo rmmod ioctl_basics
```

## 11 — concurrency_locking

```bash
cd 11_concurrency_locking && make
sudo insmod ./concurrency_locking.ko
sudo ./stress_test 8 20000
```
**Look for:** `RACE DETECTED: N update(s) were lost` — mode defaults to
unsynchronized. Then fix it and rerun:
```bash
echo 3 | sudo tee /sys/class/misc/race_demo/mode   # atomic
sudo ./stress_test 32 50000
```
**Look for:** `expected=... actual=... lost=0` / `no lost updates`.
```bash
sudo rmmod concurrency_locking
```

## 12 — wait_queues_blocking

```bash
cd 12_wait_queues_blocking && make
sudo insmod ./wait_queues_blocking.ko
time cat /dev/blocking_demo
```
**Look for:** the `cat` genuinely blocks (real time ~0–3s depending on
timing) before printing `event #1` — this is a real blocking `read()`,
not an instant return.
```bash
echo 1 | sudo tee /sys/class/misc/blocking_demo/trigger
```
**Look for:** a backgrounded, already-blocked `cat` (`cat
/dev/blocking_demo &`) returns immediately when you write to `trigger`.
```bash
sudo rmmod wait_queues_blocking
```

## 13 — kernel_memory

```bash
cd 13_kernel_memory && make
sudo insmod ./kernel_memory.ko
dmesg | tail -3   # this kernel's real KMALLOC_MAX_SIZE
MAX=$(grep -oP 'kmalloc_max_size=\K[0-9]+' /sys/kernel/kernel_memory/stats)
echo "kmalloc $((MAX * 2))" | sudo tee /sys/kernel/kernel_memory/allocate
```
**Look for:** `tee: ... Cannot allocate memory` — a request past
`KMALLOC_MAX_SIZE` always fails. Then the exact same size with `vmalloc`:
```bash
echo "vmalloc $((MAX * 2))" | sudo tee /sys/kernel/kernel_memory/allocate
cat /sys/kernel/kernel_memory/info
```
**Look for:** this one **succeeds** — the concrete kmalloc-vs-vmalloc
size-ceiling difference, not just asserted in prose.
```bash
echo 1 | sudo tee /sys/kernel/kernel_memory/free
sudo rmmod kernel_memory
```

## 14 — timers_workqueues

*(Safe to run anywhere the rest of this repo is; only the optional
"deliberately unsafe" exercise in this lab's own README needs the VM.)*
```bash
cd 14_timers_workqueues && make
sudo insmod ./timers_workqueues.ko
watch -n 1 cat /sys/kernel/timers_workqueues/stats
```
**Look for:** both `timer_ticks` and `work_ticks` climbing. Compare
`timer_last_ctx` (`in_softirq=1 in_task=0`, `comm=` jumps around between
whatever task happened to be running) against `work_last_ctx`
(`in_softirq=0 in_task=1`, `comm=kworker/...` consistently) — this
contrast *is* the lab.
```bash
sudo rmmod timers_workqueues
```

## 15 — kthreads

```bash
cd 15_kthreads && make
sudo insmod ./kthreads.ko
cat /sys/kernel/kthreads_demo/status
sleep 2 && cat /dev/kthread_demo
echo stop | sudo tee /sys/kernel/kthreads_demo/control
cat /sys/kernel/kthreads_demo/status
```
**Look for:** `running=1` with a real `pid` initially; after `stop`,
`running=0 pid=-1`. `cat /dev/kthread_demo` drains whatever the producer
thread built up in the meantime as `seq=N ns=...` lines.
```bash
sudo rmmod kthreads
```

## 16 — debugfs_sysfs

```bash
cd 16_debugfs_sysfs && make
sudo insmod ./debugfs_sysfs.ko
echo 1 | sudo tee /sys/kernel/debugfs_sysfs_demo/increment
dmesg | tail -2
echo 9999 | sudo tee /sys/kernel/debug/debugfs_sysfs_demo/counter_raw
cat /sys/kernel/debugfs_sysfs_demo/counter
```
**Look for:** the sysfs `increment` write logs a `dmesg` line; the
debugfs `counter_raw` write does **not** log anything, yet both are
visibly the same underlying variable (`counter` jumps straight to
`9999`). That silent-vs-logged, validated-vs-raw contrast is the lab.
```bash
sudo rmmod debugfs_sysfs
```

## 17 — next_steps

No module to run — read [`17_next_steps/README.md`](17_next_steps/README.md)
once the rest of this feels solid, and pick one thread (interrupts is the
recommended first one — it extends lab 03 directly).

---

## If something doesn't match this document

The per-lab `README.md` is the source of truth over this file for
anything more detailed than what's here (exact sysfs paths, full command
explanations, more exercises) — this runbook intentionally compresses
each lab down to "build, run, watch for X." If a `make` or `insmod` step
fails, check that lab's README first; if the *expected output* doesn't
match what's described here, that's worth treating as a real bug report
against this repository, not just something to work around.
