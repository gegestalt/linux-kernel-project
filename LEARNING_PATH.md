# Learning path: beginner → intermediate → advanced → pro

This repo's 17 labs are a real, hands-on foundation — but they're a
foundation, not the whole building. This document maps what labs 01–16
actually cover onto skill level, and for each level, points at real,
freely-available material to study *next*, once a level's labs stop
teaching you anything new. `17_next_steps/README.md` already maps
specific *topics* (interrupts, platform drivers, DMA, ...) to what's
missing from this repo; this document is the complementary map from
*skill level* to *where the professionals actually learned it*, with
verified sources, not a reading list assembled from memory.

## Beginner — labs 01–04

`01_hello_init` → `02_better_hello` → `03_gpio_sim` → `04_module_params`.

This band is entirely about the *mechanics* of a module existing at
all: the two generations of entry-point macros, one large synthesis
module (03) previewing where everything else is headed, and parameters
in isolation. If you can explain *why* `02_better_hello` uses
`module_init()`/`__init` instead of `01_hello_init`'s raw
`init_module()`, and what `/sys/module/*/parameters/` is actually
backed by, you're through this band.

**Next, once this band feels easy:**

- **[Linux Device Drivers, 3rd Edition](https://lwn.net/Kernel/LDD3/)**
  (Corbet, Rubini, Kroah-Hartman) — hosted free by LWN.net under
  CC-BY-SA 2.0, all 18 chapters as PDF, plus one combined 11MB tarball.
  It's current as of kernel 2.6.10 — the *APIs* it teaches (module
  basics, char drivers, concurrency, memory) have mostly aged well even
  though exact function signatures have drifted; read chapters 1–3 now
  (module basics, char drivers) as the same material this band covers,
  told by the people who wrote large parts of the kernel it's
  documenting.
- **[Bootlin's free Linux kernel and driver development training](https://bootlin.com/training/kernel/)**
  — a real, paid 5-day professional training course whose slides and
  lab guides are published free (CC-BY-SA 3.0) specifically so anyone
  can self-study them. The slide PDF is at
  [bootlin.com/doc/training/linux-kernel/linux-kernel-slides.pdf](https://bootlin.com/doc/training/linux-kernel/linux-kernel-slides.pdf).
  Their labs target real embedded boards (BeagleBone Black, i.MX93);
  this repo's `gpio-sim` approach is the "no hardware needed" version
  of the same exercises.

## Intermediate — labs 05–10

`05_register_cdev` → `06_procfs_seqfile` → `07_log_level` →
`08_open_release_cdev` → `09_read_write_cdev` → `10_ioctl_basics`.

This band is the actual VFS/character-device contract: registration,
`/proc`, the `struct file` lifecycle, real read/write with the
user/kernel boundary, and custom control operations. It's also where
`checkpatch.pl` first gets wired into a `Makefile` (lab 05) — notice
that and keep it running for the rest of your own code from here on,
not just inside this repo.

**Next:**

- **LDD3 chapters 3 (char drivers, deeper), 6 (advanced char driver
  operations — this is literally `ioctl` and blocking I/O), and 7
  (the "time, delays, and deferred work" chapter)** — the same free
  LWN hosting as above. This band's labs are LDD3's chapters 3 and 6
  compressed and modernized.
- **`Documentation/filesystems/vfs.rst`** in `../linux_mainline` —
  read it once you've built `06_procfs_seqfile` and `09_read_write_cdev`
  yourself; it's the actual contract `struct file_operations` is
  implementing, in the kernel's own words rather than inferred from a
  lab's behavior.

## Advanced — labs 11–16

`11_concurrency_locking` → `12_wait_queues_blocking` →
`13_kernel_memory` → `14_timers_workqueues` → `15_kthreads` →
`16_debugfs_sysfs`.

This band is where "does it work" stops being the bar and "is it
*correct under concurrency and real allocation constraints*" starts
being the bar — locking primitives and their actual cost, blocking I/O
built correctly (not just "it returns eventually"), allocator choice,
execution-context rules (softirq vs. process context), and the
stability-vs-convenience tradeoff behind every debug interface you've
used in this repo (`16` vs. `03`'s sysfs).

**Next:**

- **LDD3 chapter 5 (concurrency and race conditions) and chapter 8
  (allocating memory)** — same free hosting; this is where the book's
  age matters least, since locking primitives and allocator behavior
  are some of the most stable APIs in the kernel.
- **`Documentation/locking/locktypes.rst`** in `../linux_mainline` —
  already pointed at from `11_concurrency_locking/README.md`'s "Things
  to try"; read it in full once you've felt the difference between
  `spinlock_t` and `struct mutex` yourself rather than being told it.
- **Robert Love, *Linux Kernel Development*, 3rd ed.** — not free, but
  worth naming: it's the standard reference for exactly this band's
  material (process scheduling, kernel synchronization methods, memory
  management), written by a kernel developer, and consistently the
  book recommended once someone's past the "what is a module" stage
  this repo starts at.

## Pro — the part no lab in this repo teaches

Once all 16 labs feel routine, the actual gap between "can write a
correct driver" and "is a kernel developer" is almost entirely process,
not more APIs to memorize:

- **[kernelnewbies.org/FirstKernelPatch](https://kernelnewbies.org/FirstKernelPatch)**
  — a real, currently-maintained walkthrough of the *entire* pipeline
  this repo's `checkpatch.pl` habit has been rehearsing pieces of since
  lab 05: environment setup, finding something small and real to fix,
  `scripts/get_maintainer.pl`, formatting a patch correctly, sending it
  with `git send-email` (never as an attachment — that's a hard rule,
  not a style preference), and handling review feedback. See
  [`DEPLOYMENT.md`](DEPLOYMENT.md) for this repo's own version of that
  pipeline, exercised against these exact labs.
- **[kernelnewbies.org/FAQ/WhereDoIBegin](https://kernelnewbies.org/FAQ/WhereDoIBegin)**
  — shorter, more general version of the same advice: do a little of
  various kinds of kernel work rather than picking one subsystem
  immediately, to find out what you actually want to specialize in.
- **Deployment and packaging** — every lab in this repo ends at
  `insmod`. Getting a module onto a machine that isn't yours (DKMS,
  module signing, how distros package a kernel module) is covered in
  [`DEPLOYMENT.md`](DEPLOYMENT.md), and it's the other half of "pro"
  alongside upstreaming: most real-world driver work never goes
  upstream at all, and needs this instead.
- **`17_next_steps/README.md`** — once you're picking a subsystem to
  specialize in (interrupts, platform drivers, DMA, block, netlink),
  that's the map of where each one starts from what this repo already
  taught you.

## A rough calibration

If you can sit down and, from memory, sketch what `gpioctrl_sample_once()`
in `03_gpio_sim/gpioctrl.c` does and why it's protected the way it is —
without opening the file — you're past "beginner." If you can explain
why `11_concurrency_locking`'s `MODE_NONE` is racy in terms of the
actual instruction-level window, not just "it's not locked" — you're
past "intermediate." If you've read enough of `../linux_mainline`'s own
`Documentation/` unprompted, chasing something a lab's README pointed
at, that it's started feeling like reference material instead of a wall
of text — you're past "advanced," and the honest next step is real
patches, not more labs.
