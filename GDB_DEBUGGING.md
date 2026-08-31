# GDB / KGDB kernel debugging

`dmesg -w` tells you what a module *decided to say*. GDB tells you what
it actually *did* — every variable, every branch taken, every call into
another subsystem's code, one line at a time, on the real running kernel.
This document is the one-time environment setup and the general
workflow; each lab's own `README.md` has a short **Debugging with GDB**
section at the end with the specific breakpoints and things to look at
for that lab.

Based on the official kernel documentation:
<https://www.kernel.org/doc/html/latest/dev-tools/gdb-kernel-debugging.html>

## Why KGDB-over-serial, not a hypervisor gdbstub

QEMU has a built-in gdbstub (`-s`) that's the simplest possible setup —
if you ever run a lab under QEMU, prefer it. But this repo's environment
is VMware/Apple Virtualization, and neither exposes an equivalent
first-class stub the way QEMU does (VMware has a proprietary one that's
fiddlier to wire up; AVF has none at all). **KGDB-over-serial** lives
entirely inside the guest kernel and doesn't depend on hypervisor
support at all, so it's the one method that works identically on
VMware, AVF/UTM, real hardware, or QEMU.

## A scoping note before you start

Every lab in this repo so far has been built as an *out-of-tree module*
against your distro's prebuilt kernel headers
(`/lib/modules/$(uname -r)/build`). Those never ship a `vmlinux` with
debug symbols — distro header packages are headers only. GDB-level
kernel debugging needs a `vmlinux` you built yourself, with debug info,
that's the exact kernel you're booting. You already have the source for
this in `linux_mainline/` — this is the point where you actually build
and boot it, which is a bigger step than anything else in this repo.
Budget real time and disk space for it.

## 1. Build a debug kernel

```bash
cd linux_mainline
cp /boot/config-$(uname -r) .config   # or: make defconfig
make menuconfig
```

Enable, under *Kernel hacking*:

| Option | Why |
|---|---|
| `CONFIG_DEBUG_INFO=y` (older kernels) / `CONFIG_DEBUG_INFO_DWARF4=y` (a *choice* on 6.x) | Full DWARF debug info in `vmlinux`. Without it GDB can't map addresses to source lines. Avoid `CONFIG_DEBUG_INFO_REDUCED` — it strips type info the `lx-*` scripts need. |
| `CONFIG_GDB_SCRIPTS=y` | Generates `scripts/gdb/vmlinux-gdb.py` + the `lx-*` Python helpers, and a `vmlinux-gdb.py` symlink at the top of your build directory. |
| `CONFIG_KGDB=y` | The in-kernel debug stub. |
| `CONFIG_KGDB_SERIAL_CONSOLE=y` (or equivalent, under the KGDB submenu — exact wording drifts a little by version) | Gives you the `kgdboc=` boot parameter. |
| `CONFIG_MAGIC_SYSRQ=y` | Trigger a break-in on demand (`sysrq-g`) instead of freezing every boot with `kgdbwait`. |
| `CONFIG_FRAME_POINTER=y` | Extra help for stack unwinding alongside the ORC unwinder. |
| `CONFIG_KALLSYMS=y`, `CONFIG_KALLSYMS_ALL=y` | Usually already on; needed for symbol resolution generally. |

```bash
make -j$(nproc)
sudo make modules_install install   # inside the VM you intend to debug
```

## 2. Wire up a dedicated serial port for KGDB

Keep your normal console (`ttyS0`/`hvc0`) untouched; add a second,
dedicated port for the debugger:

- **VMware**: VM Settings → Add → Serial Port → *Output to named pipe*,
  "This end is the server", "The other end is an application", enable
  *Yield CPU on poll*. On the host, bridge the pipe to something GDB can
  attach to:
  ```bash
  socat -d -d PTY,link=/tmp/kgdb-pty,raw,echo=0 PIPE:/path/to/vmware/pipe
  ```
- **Apple Virtualization / UTM**: on the native AVF backend, add a
  serial device backed by a Unix socket or pty (the exact UI depends on
  the tool). If that's painful, switch that one VM to UTM's **QEMU**
  backend instead — same guest kernel, same KGDB config, and you also
  get QEMU's `-s` gdbstub as a fallback.
- **Escape hatch**: `qemu-system-x86_64 -kernel .../bzImage -serial pty
  ...` and point GDB at whatever pty QEMU prints. Zero hypervisor-serial
  plumbing, useful if VMware/AVF wiring becomes the bottleneck rather
  than the kernel debugging itself.

Guest kernel command line (for iterative work — **not** `kgdbwait`,
which halts boot until GDB attaches every single time):

```
kgdboc=ttyS1,115200
```

Trigger a break-in on demand, right before whatever you want to catch:

```bash
echo g | sudo tee /proc/sysrq-trigger
```

## 3. Attach and load symbols

```bash
cd linux_mainline           # your build directory
gdb vmlinux
```

```gdb
(gdb) set auto-load safe-path /        # or add-auto-load-safe-path <builddir>, once, in ~/.gdbinit
(gdb) target remote /tmp/kgdb-pty      # or host:port, per your serial bridge
(gdb) lx-version                        # sanity check: matches the running kernel?
```

`vmlinux`'s own symbols work immediately (`break do_init_module`, `bt`,
...). An out-of-tree `.ko` has none yet — its code lives at a
runtime-only address the kernel picked when it `vmalloc()`'d module
memory in, different on every `insmod`.

**Manual symbol loading** (worth doing once to understand the mechanism):

```bash
# in the guest, after insmod:
cat /sys/module/<modname>/sections/.text
cat /sys/module/<modname>/sections/.data
cat /sys/module/<modname>/sections/.bss
```
```gdb
(gdb) add-symbol-file /path/to/module.ko 0xffffffffc0012000 \
        -s .data 0xffffffffc0015000 -s .bss 0xffffffffc0015800
```

**What you'll actually use — `lx-symbols`:**

```gdb
(gdb) lx-symbols /home/adiopocere/Desktop/codes/linux-kernel-project
```

Does the `/sys/module/.../sections/*` lookup and `add-symbol-file` for
every currently-loaded module under that path, and arms a hook so any
module you `insmod` *afterward* gets its symbols loaded automatically.
Run it once near the top of a session; re-run it any time you need to
pick up a module that was loaded before the hook was armed (e.g. right
after breaking in `do_init_module`, before the new module's own `init`
has returned).

## 4. General breakpoint pattern for "break inside my module's init"

Function-name breakpoints only resolve once GDB has that function's
symbol — which for a `.ko` means *after* `lx-symbols` has seen it. So
the sequence for catching your own `init` function is:

```gdb
(gdb) break do_init_module      # generic hook every module's init runs through — always resolvable
(gdb) continue
```
```bash
# guest:
sudo insmod ./your_module.ko
```
```gdb
# GDB stops in do_init_module; the new module's sections are already mapped:
(gdb) lx-symbols /home/adiopocere/Desktop/codes/linux-kernel-project
(gdb) break your_module_init_function_name
(gdb) continue
```

From here: `next`/`step` line by line, `print <var>`, `bt`, `finish` to
run to the end of the current function, `watch <var>` to stop the moment
something changes.

Once a module's symbols are loaded, you can also break directly on any
of its other functions (`break rw_write`, `break gpioctrl_sample_once`,
...) without repeating the `do_init_module` dance — only the *first*
function GDB needs to resolve for a not-yet-loaded module needs this.

**Before you detach**, always `delete` breakpoints (or `continue` past
them). A breakpoint left armed with GDB disconnected will hang the guest
indefinitely waiting for a debugger that isn't there.

## 5. Useful `lx-*` helpers

The direct answer to "`dmesg -w` isn't enough": **`lx-dmesg`** reads the
ring buffer straight out of kernel memory, so it works *while the guest
is frozen at a breakpoint* — no live system required.

```gdb
(gdb) lx-dmesg                        # full ring buffer, offline, at this exact frozen instant
(gdb) lx-lsmod                        # confirm your module is loaded, refcount, dependents
(gdb) lx-ps                           # every task — find which process/kworker you're stopped in
(gdb) print *$lx_current()            # full task_struct of whatever's currently stopped
(gdb) lx-device-list-class("misc")    # walk the driver model - confirm a misc device registered
(gdb) lx-cpus                         # per-CPU state
(gdb) lx-version                       # confirm vmlinux matches the running kernel
```

`lx-version` first, every session — attaching to a `vmlinux` that
doesn't exactly match the running kernel is the single most common KGDB
mistake, and it fails in confusing ways (wrong line numbers, "optimized
out" everywhere) rather than a clean error.

## A note on function calls from GDB

Standard KGDB targets generally don't support calling arbitrary kernel
functions from the GDB prompt the way a userspace GDB session can
(`print some_function(x)`), and doing so on a live kernel is a good way
to wedge it if it *does* appear to work. Stick to reading state
(`print`, `x/`, `watch`) rather than calling into kernel code from the
debugger.
