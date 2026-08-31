# 17 — next_steps

No module here. Labs 01–16 built up a real toolkit — module lifecycle,
character devices, `/proc` and sysfs and debugfs, module parameters,
ioctl, locking, blocking I/O, memory allocators, timers, workqueues,
kthreads — entirely on virtual hardware (`gpio-sim`) or no hardware at
all. That toolkit is genuinely most of what a Linux driver is made of.
What's still missing is everything that only shows up once real (or
realistically emulated) hardware and a wider kernel are involved. This is
a map of where to go next, not a lab to complete.

## Interrupts

Every lab so far used *polling* (lab 03's workqueue) or *software-driven*
wakeups (lab 12's timer) instead of a real hardware interrupt. The next
concept to learn is `request_irq()`/`devm_request_irq()`, top halves vs
bottom halves (softirq/tasklet/threaded IRQ), and why an interrupt
handler has even tighter constraints than lab 14's timer callback (can't
even call `might_sleep()`-safe things unless it's a *threaded* IRQ
handler).

- Read: `Documentation/core-api/genericirq.rst`,
  `Documentation/kernel-hacking/hacking.rst` §"Interrupt handlers", in
  `../../linux_mainline`.
- `gpio-sim` lines can generate real interrupts (`gpiod_to_irq()`) — this
  is the most natural next lab to build yourself: extend `03_gpio_sim`'s
  button-watching design to react to a GPIO edge interrupt instead of
  polling on a timer, and compare latency/CPU-usage against the polling
  version.

## Platform drivers and Device Tree

Every driver in this repo binds to hardware it finds by hand
(`gpio_device_find_by_label()` in lab 03). Real embedded drivers instead
register a `struct platform_driver` and let the kernel *match* them to a
device described in the Device Tree (or, on x86, ACPI) — `probe()`/
`remove()` replace `module_init()`/`module_exit()`'s role for
hardware-specific setup.

- Read: `Documentation/driver-api/driver-model/platform.rst`,
  `Documentation/devicetree/usage-model.rst`.
- Practice without real hardware: QEMU's `virt` machine plus a hand-written
  device tree overlay, or `gpio-sim`'s own devicetree-node syntax (see
  `03_gpio_sim/README.md`'s configfs walkthrough — the same simulated chip
  can be described in DT instead) is enough to write and load a genuine
  `platform_driver` end to end.

## DMA

None of these labs move data without the CPU touching every byte
(`copy_to_user()`/`copy_from_user()` throughout). Real high-throughput
drivers (network, storage, GPU) use DMA engines instead, with a
completely different memory model: `dma_alloc_coherent()`,
streaming DMA mappings, and cache coherency concerns that don't exist
anywhere in this repo.

- Read: `Documentation/core-api/dma-api.rst`,
  `Documentation/core-api/dma-api-howto.rst`.
- This one genuinely needs real (or well-emulated) DMA-capable hardware;
  QEMU's `edu` PCI device (see below) is a commonly-used practice target
  with a real DMA engine you can drive from a toy driver.

## Getting from gpio-sim to QEMU

Every lab here runs directly against your live kernel. The natural next
step for "hardware" that doesn't literally exist is QEMU:

- `qemu-system-x86_64 -M q35 -device edu` gives you a fake PCI device
  (the "edu" device, built specifically for driver-writing practice) with
  MMIO registers, an interrupt, and a small DMA engine — a complete,
  safe target for a first PCI driver, including `pci_driver` registration
  (the PCI equivalent of the platform-driver model above) and
  `pci_iomap()`/`ioread32()`/`iowrite32()` for MMIO access.
- Booting your *own* kernel build under QEMU (rather than modules against
  the host kernel, as every lab here does) is also worth doing once: it's
  the only way to safely experiment with changes that could hang or crash
  a machine outright, and it decouples your kernel version from whatever
  the host happens to run.

## Block drivers

Character devices (every `/dev` node in this repo) are the simpler half
of the story. A block device (`struct block_device_operations`, `struct
request_queue`/`blk-mq`) has to handle request queuing, I/O scheduling,
and partitioning — a genuinely different, more complex model worth
learning once character devices feel solid.

- Read: `Documentation/block/index.rst`.
- `drivers/block/null_blk/` in `../../linux_mainline` is a real,
  hardware-free block driver you can load, benchmark, and read start to
  finish.

## Networking: netlink

Every control-plane interface built here (sysfs, debugfs, ioctl, `/proc`)
is synchronous request/response. Netlink sockets are how the kernel talks
to userspace *asynchronously and by broadcast* — think `ip link`,
`udevadm monitor`, or `NetworkManager` watching for interface changes
live. Worth learning if you're headed toward network drivers or any
driver that needs to push unsolicited events to multiple listeners.

- Read: `Documentation/userspace-api/netlink/intro.rst`.

## Debugging and observability beyond dmesg/sysfs

- **ftrace** (`/sys/kernel/debug/tracing/`) — function-level tracing of
  the *running* kernel, including this repo's own modules, with no
  instrumentation code needed. Try: `echo function >
  /sys/kernel/debug/tracing/current_tracer`, then
  `set_ftrace_filter` narrowed to one of this repo's functions.
- **kprobes** — dynamically breakpoint (nearly) any kernel function,
  including your own modules', from a small kernel module of their own.
- **KUnit** (`Documentation/dev-tools/kunit/`) — the kernel's own
  in-tree unit test framework; worth trying against a small, pure-logic
  function factored out of one of these labs (e.g., `04_module_params`'s
  clamping logic) as a first KUnit test.
- **sparse** and **smatch** — static analyzers built for exactly kernel
  code's patterns (`__user` pointer misuse, endianness bugs, locking
  imbalance). Try `make C=2` in any lab directory with `sparse` installed.

## Where the checkpatch habit leads

Labs 05, 07, 08, and 09–16 all wire `checkpatch.pl --strict` into their
Makefile. The natural extension of that habit is actually sending a patch
upstream once — even a trivial one (`Documentation/process/submitting-patches.rst`
in `../../linux_mainline`) — which exercises the *whole* pipeline this
repo has been rehearsing pieces of: correct commit messages,
`scripts/get_maintainer.pl`, `git send-email`, and review feedback.

## A rough order

If picking just one next thing: **interrupts** (extends lab 03 directly,
no new hardware needed beyond `gpio-sim`), then **platform drivers +
devicetree** (the natural home for an interrupt-driven `gpio-sim` driver
to live in properly, replacing the manual `gpio_device_find_by_label()`
lookup), then **QEMU's `edu` device** once you want real MMIO/DMA/PCI
without needing physical hardware.
