# GDB / KGDB kernel debugging

`dmesg -w` tells you what a module *decided to say*. GDB tells you what
it actually *did* — every variable, every branch taken, every call into
another subsystem's code, one line at a time, on the real running kernel.
This document is the environment setup and the general workflow; each
module's own `readme.md` has a short **Debugging with GDB** section with
the specific breakpoints for it, and the quick-reference table
near the end of this file lists all of them in one place for a live
session.

Based on the official kernel documentation:
<https://www.kernel.org/doc/html/latest/dev-tools/gdb-kernel-debugging.html>

## Status: the debug kernel is built

`linux_mainline/vmlinux` exists, is `ELF ... with debug_info, not
stripped`, and `include/config/kernel.release` reports
**`7.2.0-kgdb-debug+`** — distinct from this machine's actual running
kernel, so it can never collide with it. `arch/arm64/boot/Image` (the
bootable kernel image) is built too. Section 2 below is now a record of
how it was built, not something you need to repeat.

**Use the QEMU path below — it's simpler, needs no second VM, and is the
one actually verified working end-to-end this session** (real breakpoint
hit, real `lx-symbols`, real single-step, real `finish` return value, on
module 01). Sections 0/1/3/4 (the two-VM VMware plan) are kept as a
documented alternative — useful if you specifically want a second,
independent machine, or QEMU's software emulation (no `/dev/kvm` on this
VM, so it runs unaccelerated — still fine for debugging, just not fast)
turns out to be a problem for you — but they're no longer the
recommended path.

## QEMU path (recommended): the debug kernel, right here, no second VM

QEMU supplies its own gdbstub (`-s`), so the isolation this workflow
fundamentally needs — something has to freeze at a breakpoint without
taking down the machine GDB runs on — comes from the QEMU *process*
being a separate, disposable thing GDB controls from outside, not from a
second VM. This machine (VM A) never freezes; only the QEMU guest does.

### One-time setup: a busybox initramfs and a scratch disk

```bash
mkdir -p /home/adiopocere/Desktop/codes/qemu-vmb/initramfs_root/{bin,dev,proc,sys,mnt/labs}
cd /home/adiopocere/Desktop/codes/qemu-vmb/initramfs_root
cp /usr/bin/busybox bin/busybox
for applet in $(./bin/busybox --list); do
  [ "$applet" = busybox ] && continue   # don't symlink over the real binary with itself
  ln -sf busybox "bin/$applet"
done
```

`/init` (make executable with `chmod +x`):

```bash
#!/bin/busybox sh
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev 2>/dev/null || /bin/busybox mdev -s
/bin/busybox mdev -s
mkdir -p /mnt/labs

# /dev/vda can take a moment to appear - virtio-blk enumeration isn't
# guaranteed to be done by the time devtmpfs mounts. Without this loop,
# the very next line fails intermittently with "Can't lookup blockdev"
# and every insmod after that gets a plain "No such file or directory"
# with nothing pointing at the real cause - hit for real building this.
i=0
while [ ! -b /dev/vda ] && [ $i -lt 20 ]; do
  sleep 0.2
  /bin/busybox mdev -s
  i=$((i+1))
done

mount -t ext4 /dev/vda /mnt/labs
echo
echo "=== VM B (QEMU) ready ==="
echo "lab modules are at /mnt/labs (virtio-blk disk from the host)"
echo "kernel: $(uname -r)"
echo
exec /bin/busybox setsid /bin/busybox cttyhack /bin/busybox sh
```

If you already built the initramfs before this fix and `insmod` fails
with `No such file or directory` even though the file is really there
(check from the host: `sudo mount -o loop labs-disk.img /tmp/x && ls
/tmp/x/NN_module_name/ && sudo umount /tmp/x`), this is why — rebuild the
initramfs with the loop above and reboot the guest.

Package it, and create a small scratch disk you'll copy each module's
built `.ko` onto:

```bash
cd /home/adiopocere/Desktop/codes/qemu-vmb/initramfs_root
find . | cpio -o -H newc 2>/dev/null | gzip > ../initramfs.cpio.gz

cd /home/adiopocere/Desktop/codes/qemu-vmb
qemu-img create -f raw labs-disk.img 64M
mkfs.ext4 -q -F labs-disk.img
```

(9p passthrough would avoid the copy-in-a-disk-image step entirely, but
`CONFIG_NET_9P`/`CONFIG_9P_FS` weren't in the trimmed kernel config — not
worth a rebuild for. `CONFIG_VIRTIO_BLK`/`CONFIG_EXT4_FS` were already
built in, so a disk image was the zero-rebuild option.)

### tmux layout: set this up once, reuse for every module

One session, two side-by-side panes — `vmb` (left) runs QEMU, `gdb`
(right) runs gdb. Set this up once per sitting, not once per module —
you'll reboot the guest between modules, but there's no need to tear
down and recreate the tmux session itself each time. This is the actual
layout this whole workflow was built and verified against; every
"in the vmb pane" / "in the gdb pane" instruction elsewhere in this repo
means these two panes.

```bash
tmux kill-session -t kgdb 2>/dev/null
tmux new-session -d -s kgdb -x 220 -y 50
tmux split-window -h -t kgdb
tmux set -g mouse on
tmux select-pane -t kgdb:0.0 -T vmb
tmux select-pane -t kgdb:0.1 -T gdb
tmux set -t kgdb pane-border-status top
tmux attach -t kgdb
```

The first line is a clean-slate kill of any old session of the same
name — safe to run even if nothing exists yet (`2>/dev/null` swallows
the "no such session" error), and it means you can re-run this whole
block any time you want to start completely fresh without first
figuring out what's still running. After `attach`, you have a real,
clickable two-pane terminal: click either pane to focus it (mouse is
on), or `Ctrl-b` then an arrow key. Closing your terminal tab does
**not** kill this session — tmux keeps it running in the background;
`tmux attach -t kgdb` from any terminal gets you back into the exact
same state, panes and all. Nothing here actually ends until you
`poweroff -f` the guest and/or run `tmux kill-session -t kgdb`
yourself.

### Per-module: build against the debug kernel, not the host's

Every module's Makefile hardcodes `/lib/modules/$(uname -r)/build` — that
builds against *this machine's* running kernel, producing a `.ko` whose
`vermagic` won't match the debug kernel and that the guest will refuse
to load. Invoke kbuild directly instead, pointing at the debug tree:

```bash
cd NN_module_name
make -C /home/adiopocere/Desktop/codes/linux_mainline M=$(pwd) modules
modinfo *.ko | grep vermagic   # should read 7.2.0-kgdb-debug+, not this host's kernel
```

Copy the result onto the scratch disk:

```bash
sudo mount -o loop /home/adiopocere/Desktop/codes/qemu-vmb/labs-disk.img /tmp/vmb-mnt
sudo mkdir -p /tmp/vmb-mnt/NN_module_name
sudo cp *.ko /tmp/vmb-mnt/NN_module_name/
sudo umount /tmp/vmb-mnt
```

### Boot the guest (in `vmb`) and attach GDB (in `gdb`)

In the `vmb` pane (left) of the `kgdb` tmux session set up above:

```bash
qemu-system-aarch64 -M virt -cpu max -m 1024 -smp 2 \
  -kernel /home/adiopocere/Desktop/codes/linux_mainline/arch/arm64/boot/Image \
  -initrd /home/adiopocere/Desktop/codes/qemu-vmb/initramfs.cpio.gz \
  -drive file=/home/adiopocere/Desktop/codes/qemu-vmb/labs-disk.img,if=virtio,format=raw \
  -append "console=ttyAMA0 rdinit=/init nokaslr" -nographic -s
```

Wait for `=== VM B (QEMU) ready ===` and a `~ #` prompt before touching
the `gdb` pane.

**`nokaslr` is not optional.** `CONFIG_RANDOMIZE_BASE=y` is set, and
without disabling it at boot, every address GDB reads out of the static
`vmlinux` symbol table is offset from where the kernel actually runs —
breakpoints get silently planted at the wrong address and simply never
fire (this happened on the very first attempt this session: `insmod`
ran straight through with no stop, no error, nothing — the quietest
possible failure mode). No kernel rebuild needed, it's a boot parameter.

In the `gdb` pane (right):

```bash
cd /home/adiopocere/Desktop/codes/linux_mainline
gdb -q -iex 'set auto-load safe-path /' vmlinux
```

**The `-iex 'set auto-load safe-path /'` matters too** — plain `set
auto-load safe-path /` typed *after* `gdb vmlinux` has already started is
too late: GDB tries to auto-load `scripts/gdb/vmlinux-gdb.py` (which is
what makes `lx-symbols`/`lx-dmesg`/etc. exist at all) while starting up,
*before* your first typed command runs, declines it as unsafe, and
manually `source`-ing the script afterward as a workaround breaks its
own internal `import linux` (the script expects the auto-loader's own
path setup, not a bare manual `source`). `-iex` runs before the file
loads, so auto-load succeeds normally.

One command per line — see rule 3 above:

```gdb
target remote :1234
break do_init_module
continue
```

Then in the `vmb` pane:

```bash
insmod /mnt/labs/NN_module_name/your_module.ko
```

```gdb
# back in the gdb pane - real output from this session, module 01:
Thread 2 hit Breakpoint 1, do_init_module (mod=mod@entry=0xffff80007c322040) at kernel/module/main.c:3089
3089	{
(gdb) lx-symbols /home/adiopocere/Desktop/codes/linux-kernel-project
loading @0xffff80007c320000: /home/adiopocere/Desktop/codes/linux-kernel-project/01_hello_init/hello.ko
(gdb) break init_module
Breakpoint 2 at 0xffff80007c320010: file hello.c, line 10.
(gdb) continue
Thread 2 hit Breakpoint 2, init_module () at hello.c:10
10	    printk(KERN_INFO "Hello luv .\n");
(gdb) next
16	    return 0;
(gdb) finish
Value returned is $1 = 0
```

From here it's the same general pattern as any KGDB session — see
section 6 below. To end a session: in the `gdb` pane, `delete` your
breakpoints (name their numbers — see rule 3), then in the `vmb` pane
run `poweroff -f`. The `kgdb` tmux session itself can stay up — reboot
the guest fresh for the next module rather than tearing down and
recreating the tmux layout each time (a fresh boot is cheap, a few
seconds). Only run `tmux kill-session -t kgdb` if you actually want to
close the whole layout down.

### Three rules that cause real, confusing-looking failures if missed

Both were hit for real, live, running exactly this walkthrough:

1. **`continue` means "go trigger the next thing in the other pane" —
   don't keep typing in `gdb`.** Commands typed into `gdb` right after a
   `continue` just sit in a buffer, unexecuted, until whatever you were
   supposed to do on the guest side actually happens. If you type ahead
   (e.g. `break cleanup_module` / `continue` / `bt` / `next` / `finish`
   all at once without ever switching to the guest pane to run `rmmod`
   in between), nothing you typed after the first `continue` does
   anything, and it looks like GDB just stopped responding.

2. **`Ctrl-C` in GDB freezes the entire guest, not just the debugger
   prompt.** KGDB-style interrupts stop the actual (emulated) CPU. Once
   you've interrupted, the guest pane will look completely dead — even a
   bare `Enter` does nothing — until you go back to `gdb` and `continue`
   again. This is easy to mistake for a broken terminal; it's the guest
   correctly waiting for you.

3. **Paste exactly one command at a time into `gdb` — never a
   multi-line block.** This one is easy to dismiss until it happens to
   you: depending on your terminal, pasting several lines at once can
   get buffered as a *single* input instead of one command per line, and
   gdb's readline will happily concatenate them into one bogus command.
   Confirmed live, this exact paste —
   ```
   break events_seq_start
   break events_seq_next
   break events_seq_show
   break events_seq_stop
   continue
   ```
   — did not set four breakpoints and continue. It produced one:
   `Function "events_seq_start\n  break events_seq_next\n  break
   events_seq_show\n  break events_seq_stop\n  continue" not defined`,
   followed by a bogus *pending* breakpoint on a function whose "name"
   is the whole garbled block. The same thing happened one step earlier
   to a `cd ... / gdb ... / target remote :1234 / break do_init_module /
   continue` paste: `target remote` never actually ran, so the target
   was never connected, and the next command (`lx-symbols`) failed with
   `Python Exception <class 'gdb.error'>: No registers.` — a confusing
   error that has nothing to do with symbols and everything to do with
   there being no live target yet. Every multi-line `gdb` block in this
   repo's docs (this file included) is written to be read top to bottom,
   **one line pasted or typed, Enter, wait for `(gdb)` to reappear, then
   the next line** — never select-and-paste the whole block. The same
   applies to `vmb`: one shell command at a time.

If a session ever gets into a confusing state, `info breakpoints` in
`gdb` and a fresh `capture-pane`-style look at both panes' actual
scrollback (not just what's visible right now) will show you exactly
what's pending — a stuck `Continuing.` with no matching hit means
something on the guest side was never triggered, or the guest itself is
still frozen from an earlier interrupt, or (per rule 3) a paste got
mangled and nothing you think you sent actually ran.

## Smoke test: verify the whole setup, start to finish

Run this once, using module 01 (the simplest one — no extra setup like
gpio-sim), before trying to debug anything else. If every step below
produces the output shown, the entire chain works end to end: the build
against the debug tree, getting a `.ko` onto the guest, the QEMU boot,
the gdbstub connection, a KGDB breakpoint actually firing, module symbol
loading, single-stepping, and reading a return value. Anything that
breaks past this point is specific to whatever module you're debugging,
not the environment itself.

**Every numbered step here is exactly one command.** Send it, wait for
the prompt (`(gdb)` or `~ #`) to reappear, read the output, then move to
the next step. Do not paste more than one step at a time — see rule 3
above for exactly what goes wrong if you do.

0. Set up the tmux layout (the "tmux layout" section above) if you
   haven't already — you need the `vmb` and `gdb` panes open before step
   4.
1. Build module 01 against the debug tree:
   ```bash
   cd 01_hello_init
   make -C /home/adiopocere/Desktop/codes/linux_mainline M=$(pwd) modules
   ```
2. Confirm the vermagic matches the debug kernel, not your host's:
   ```bash
   modinfo hello.ko | grep vermagic
   ```
   Expect `7.2.0-kgdb-debug+ ...`. If it reports your host's own kernel
   version instead, the module was built against the wrong tree — stop
   here and fix the build command before continuing.
3. Copy it onto the scratch disk:
   ```bash
   sudo mount -o loop /home/adiopocere/Desktop/codes/qemu-vmb/labs-disk.img /tmp/vmb-mnt
   sudo mkdir -p /tmp/vmb-mnt/01_hello_init
   sudo cp hello.ko /tmp/vmb-mnt/01_hello_init/
   sudo umount /tmp/vmb-mnt
   ```
4. In the `vmb` pane, boot the guest:
   ```bash
   qemu-system-aarch64 -M virt -cpu max -m 1024 -smp 2 \
     -kernel /home/adiopocere/Desktop/codes/linux_mainline/arch/arm64/boot/Image \
     -initrd /home/adiopocere/Desktop/codes/qemu-vmb/initramfs.cpio.gz \
     -drive file=/home/adiopocere/Desktop/codes/qemu-vmb/labs-disk.img,if=virtio,format=raw \
     -append "console=ttyAMA0 rdinit=/init nokaslr" -nographic -s
   ```
   Wait for `=== VM B (QEMU) ready ===` and a `~ #` prompt before moving on.
5. In the `gdb` pane, start gdb:
   ```bash
   cd /home/adiopocere/Desktop/codes/linux_mainline
   gdb -q -iex 'set auto-load safe-path /' vmlinux
   ```
   Expect `Reading symbols from vmlinux...` and a `(gdb)` prompt.
6. Connect to the guest:
   ```gdb
   target remote :1234
   ```
   Expect `Remote debugging using :1234` followed by a real source
   location (something like `alternative_has_cap_unlikely (...) at
   ./arch/arm64/include/asm/alternative-macros.h:254`). If you instead
   get nothing, or an error, stop here — nothing past this step will
   work until this connects (see rule 3: a mangled paste is the most
   common reason this silently fails).
7. Set the entry breakpoint:
   ```gdb
   break do_init_module
   ```
   Expect `Breakpoint 1 at 0x...: file kernel/module/main.c, line ...`.
8. ```gdb
   continue
   ```
   Expect `Continuing.` and nothing else yet — the guest is running
   again, waiting for you to trigger the breakpoint from the other pane.
9. Back in the `vmb` pane:
   ```bash
   insmod /mnt/labs/01_hello_init/hello.ko
   ```
   Don't press Enter again yet — switch to the gdb pane first.
10. In the gdb pane, confirm the hit:
    ```
    Thread 2 hit Breakpoint 1, do_init_module (mod=...) at kernel/module/main.c:3089
    ```
    This is the real proof the gdbstub connection and KGDB breakpoint
    machinery both work — you're stopped inside the live kernel, mid
    syscall, with a real module pointer.
11. Load the module's own symbols now that it's mapped in:
    ```gdb
    lx-symbols /home/adiopocere/Desktop/codes/linux-kernel-project
    ```
    Expect `loading @0x...: .../01_hello_init/hello.ko`.
12. ```gdb
    break init_module
    ```
    Expect `Breakpoint 2 at 0x...: file hello.c, line 10.` — a real
    source line in your own module now, not a kernel file.
13. ```gdb
    continue
    ```
14. Confirm the hit:
    ```
    Thread 2 hit Breakpoint 2, init_module () at hello.c:10
    10	    printk(KERN_INFO "Hello luv .\n");
    ```
15. ```gdb
    next
    ```
    Expect line `16	    return 0;` — single-stepping works.
16. ```gdb
    finish
    ```
    Expect `Value returned is $1 = 0` — reading a real return value
    works.
17. Clean up:
    ```gdb
    delete 1 2
    ```
    ```bash
    # vmb:
    rmmod hello
    ```
    ```bash
    # vmb:
    poweroff -f
    ```

If all 17 steps matched, the environment is confirmed working and
you're ready to move on to any module's own `gdb_walkthrough.md`.

## Alternative path: two VMware VMs instead of QEMU

Everything from here through section 4 is the two-VM plan — kept as a
documented fallback (see "Status" above for why QEMU is recommended
instead). Skip to section 5 if you're using the QEMU path.

## 0. The topology: two VMs, not one

This machine (call it **VM A**) is itself a VMware guest — running GDB
*and* the kernel being debugged on the same VM doesn't work: a live KGDB
breakpoint halts the entire machine, including whatever's driving GDB.
So the setup is two VMs under the same VMware installation:

```
VM A (this machine, stays up)              VM B (disposable target)
┌─────────────────────────┐    serial     ┌─────────────────────────┐
│ gdb vmlinux              │◄─────────────►│ boots the debug kernel   │
│ you type commands here   │               │ gets insmod'd, hits      │
│ never freezes            │               │ breakpoints, freezes     │
└─────────────────────────┘               └─────────────────────────┘
```

VM B is expendable on purpose: snapshot it right after setup, and revert
in one click if a module panics it or a bad `grub` entry strands it —
that's what makes it safe to actually hit breakpoints and step through
code that could, in principle, go wrong.

## 1. Create VM B

The debug kernel (section 2) is already built on VM A — clone **now**,
so VM B inherits the finished `linux_mainline/vmlinux` and
`arch/arm64/boot/Image` for free and never has to build anything itself.

In VMware: **clone VM A** (`File → Clone → Full Clone`) rather than
installing fresh — this guarantees VM B has the exact same userland,
`libgpiod`, `linux_mainline` checkout (built kernel included), and this
repo already in place. Give it a distinct name
(`linux-kernel-project-debug-target`).

**Immediately after the clone, before doing anything else:**
`VM → Snapshot → Take Snapshot`. This is the actual safety net for
everything that follows.

## 2. Build the debug kernel (already done on VM A — this is the record)

```bash
cd linux_mainline
sudo apt install -y flex bison libssl-dev libelf-dev gawk   # build prerequisites
cp /usr/src/linux-headers-$(uname -r)/.config .config          # seed from the running config
sed -i 's/^CONFIG_LOCALVERSION=.*/CONFIG_LOCALVERSION="-kgdb-debug"/' .config
```

The running kernel's own config already has everything needed
(`CONFIG_DEBUG_INFO=y`, `CONFIG_GDB_SCRIPTS=y`, `CONFIG_KGDB=y`,
`CONFIG_KGDB_SERIAL_CONSOLE=y`, `CONFIG_MAGIC_SYSRQ=y`), which is why
seeding from it rather than starting from `defconfig` saves a trip
through `menuconfig`. The `LOCALVERSION` change matters: it makes
`uname -r` for this kernel end in `-kgdb-debug`, so it's unmistakably
different from the one already installed and gets its own `/lib/modules/`
directory and its own GRUB entry rather than colliding with anything —
confirmed afterward via `cat include/config/kernel.release` reporting
`7.2.0-kgdb-debug+`.

Two things in a config seeded from a distro's own `/boot/config-*` don't
carry over cleanly to a vanilla upstream checkout, and both had to be
fixed before the build would proceed:

```bash
# The seeded config points at Canonical's own build-time cert files,
# which don't exist here - clear them, we don't need module signing:
sed -i 's/^CONFIG_SYSTEM_TRUSTED_KEYS=.*/CONFIG_SYSTEM_TRUSTED_KEYS=""/' .config
sed -i 's/^CONFIG_SYSTEM_REVOCATION_KEYS=.*/CONFIG_SYSTEM_REVOCATION_KEYS=""/' .config

# CONFIG_GENDWARFKSYMS needs libdw-dev (not installed) and is DWARF-based
# module-ABI-versioning bookkeeping, unrelated to debug info - disable it
# rather than installing a dependency for a feature we don't need:
./scripts/config --disable GENDWARFKSYMS

make olddefconfig
```

**Disk is a real constraint.** A distro `.config` seeded wholesale
builds modules for hardware this VM will never have — GPU drivers alone
(`amdgpu`, etc.) run into gigabytes — and the build ran the disk to 98%
full partway through (a failed `amdgpu.ko` link, "No space left on
device"). The fix: trim to only what this VM actually uses via
`localmodconfig`, which reads currently-loaded modules and disables
everything else as `=m`. **Load anything you specifically want kept
first** (`gpio-sim` was `modprobe`'d before this ran, specifically so
module 03 would keep working):

```bash
sudo modprobe gpio-sim
yes "" | make localmodconfig   # "yes" auto-answers "keep as module" prompts for ambiguous cases
make clean                       # reclaims everything the old, untrimmed build produced
```

This took the config from thousands of `=m` entries down to 50, all
core `=y` options (`KGDB`, `DEBUG_INFO`, `GDB_SCRIPTS`, ...) untouched
since `localmodconfig` only ever turns off unused *modules*, never
built-in features — worth double-checking after any trim:

```bash
grep -E "^CONFIG_KGDB=|^CONFIG_DEBUG_INFO=|^CONFIG_GDB_SCRIPTS=|^CONFIG_GPIO_SIM=" .config
```

```bash
make -j3      # leave one core free
```

This produces `vmlinux` (with full debug info — this is what GDB loads)
and the compressed boot image under `arch/arm64/boot/`. On this VM (4
cores, trimmed config) it finished in well under an hour; the original,
untrimmed distro-scale config was still running after several hours and
ran the disk out of space before finishing — trimming first isn't
optional in practice, it's the difference between finishing and not.

## 3. Install it on VM B, as an *additional*, non-default entry

Since the build (section 2) is already finished on VM A, **clone VM B
now, after it, not before** — the clone picks up the already-built
`linux_mainline/vmlinux` and `arch/arm64/boot/Image` for free, so VM B
never has to rebuild anything, only install what's already sitting
there. (If VM B was cloned earlier and doesn't have the finished build,
re-run section 2's `make -j3` on VM B directly instead of transferring
binaries — that sidesteps any path-dependent build artifacts.)

**On VM B:**

```bash
cd linux_mainline
sudo make modules_install
sudo make install                 # adds a new /boot entry, does NOT touch the existing default
```

Confirm the existing kernel is still the default before rebooting:

```bash
awk -F\' '/menuentry / {print $2}' /boot/grub/grub.cfg   # find the new entry's exact name
sudo grep '^GRUB_DEFAULT' /etc/default/grub               # should still point at the old kernel or "0"
```

If you want the new kernel to boot *this once* without permanently
changing the default:

```bash
sudo grub-reboot "Advanced options for Ubuntu>Ubuntu, with Linux 7.2.0-kgdb-debug+"
sudo reboot
```

(That's this build's actual `kernel.release` — confirm yours matches
with `cat /home/adiopocere/Desktop/codes/linux_mainline/include/config/kernel.release`
before using it verbatim; a rebuild with a different `LOCALVERSION` or
base commit would change it.)

`grub-reboot` only affects the next boot — if the new kernel fails to
come up cleanly, the *following* reboot falls back to the known-good
default automatically. (Or just use the snapshot from step 1 if it's
worse than that.)

## 4. Wire a serial line between VM A and VM B

**Preferred — network-backed serial**, since VM A and VM B are sibling
VMs on the same virtual network rather than host+guest: in VM B's
Settings → Serial Port, add a port set to connect **via network**
(exact wording varies by VMware version — look for "Use network" /
"Connect via network"; some versions phrase it as a Telnet URI). Set it
to listen as a server on a port, e.g. `5555`. No serial device is needed
on VM A's side at all — GDB connects to it directly over IP:

```
(gdb) target remote <VM_B_ip>:5555
```

**Fallback — named pipe**, only if you end up driving GDB from the real
physical host instead of from VM A: VM B Settings → Serial Port →
*Output to named pipe*, "This end is the server", "The other end is an
application", enable *Yield CPU on poll*. On the physical host:

```bash
socat -d -d PTY,link=/tmp/kgdb-pty,raw,echo=0 PIPE:/path/to/vmware/pipe
```

Either way, guest kernel command line on VM B (edit via `grub-reboot`'s
menu, or `/etc/default/grub`'s `GRUB_CMDLINE_LINUX` + `update-grub`, or
directly at the GRUB prompt for a one-off test):

```
kgdboc=ttyS1,115200
```

— `ttyS1`, keeping `ttyS0`/the console untouched. Trigger a break-in on
demand from VM B once it's booted:

```bash
echo g | sudo tee /proc/sysrq-trigger
```

## 5. Attach and load symbols (on VM A)

```bash
cd linux_mainline
gdb vmlinux
```

```gdb
(gdb) set auto-load safe-path /
(gdb) target remote <VM_B_ip>:5555      # or the pty path, per section 4
(gdb) lx-version                         # confirm it matches VM B's running kernel
```

`vmlinux`'s own symbols work immediately (`break do_init_module`, `bt`,
...). An out-of-tree `.ko` has none yet — its code lives at a
runtime-only address the kernel picked when it `vmalloc()`'d module
memory in, different on every `insmod`.

**Manual symbol loading** (worth doing once to understand the mechanism
— on VM B, after `insmod`):

```bash
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
every currently-loaded module under that path (on VM B, reached over
the `target remote` connection), and arms a hook so any module you
`insmod` *afterward* gets its symbols loaded automatically.

## 6. General breakpoint pattern for "break inside my module's init"

Function-name breakpoints only resolve once GDB has that function's
symbol — which for a `.ko` means *after* `lx-symbols` has seen it:

```gdb
(gdb) break do_init_module      # generic hook every module's init runs through — always resolvable
(gdb) continue
```
```bash
# on VM B:
sudo insmod ./your_module.ko
```
```gdb
# GDB stops in do_init_module; the new module's sections are already mapped:
(gdb) lx-symbols /home/adiopocere/Desktop/codes/linux-kernel-project
(gdb) break your_module_init_function_name
(gdb) continue
```

From here: `next`/`step` line by line, `print <var>`, `bt`, `finish` to
run to the end of the current function, `watch <var>` to stop the
moment something changes. Once a module's symbols are loaded, break
directly on any of its other functions without repeating this dance.

**Before you detach**, always `delete` breakpoints (or `continue` past
them) on VM B. A breakpoint left armed with GDB disconnected hangs VM B
indefinitely waiting for a debugger that isn't there.

## 7. Per-module quick reference

Every function below was already confirmed to resolve to real,
compiled debug info (`gdb -batch -ex "info line <func>" <module>.ko`) —
see each module's own README for the full trigger commands.

| # | Module | Break on |
|---|---|---|
| 01 | `hello.ko` | `init_module`, `cleanup_module` |
| 02 | `better_hello.ko` | `my_init`, `my_exit` |
| 03 | `gpioctrl.ko` | `gpioctrl_init`, `gpioctrl_sample_once`, `gpioctrl_write`, `gpioctrl_work_fn` |
| 04 | `module_params.ko` | `module_params_init`, `module_params_read`, `module_params_exit` |
| 05 | `register_cdev.ko` | `register_cdev_init`, `register_cdev_open`, `register_cdev_read` |
| 06 | `procfs_seqfile.ko` | `events_seq_start`, `events_seq_next`, `events_seq_show`, `events_seq_stop`, `info_show` |
| 07 | `printk_log_levels.ko` | `printk_log_levels_init`, `printk_emit_all_levels` |
| 08 | `open_release_cdev.ko` | `my_open`, `my_release` |
| 09 | `read_write_cdev.ko` | `read_write_cdev_init`, `rw_read`, `rw_write` |
| 10 | `ioctl_basics.ko` | `ioctl_basics_init`, `ioctl_basics_ioctl` |
| 11 | `concurrency_locking.ko` | `race_write` (not `increment_once` - compiler-inlined) |
| 12 | `wait_queues_blocking.ko` | `producer_fn`, `bq_read`, `bq_poll` |
| 13 | `kernel_memory.ko` | `allocate_store` (not `do_allocate` - compiler-inlined), `do_free` |
| 14 | `timers_workqueues.ko` | `heartbeat_timer_fn`, `heartbeat_work_fn` |
| 15 | `kthreads.ko` | `producer_thread_fn`, `start_producer`, `stop_producer`, `kthread_should_stop` |
| 16 | `debugfs_sysfs.ko` | `enabled_store`, `increment_store`, `info_read` |

## 8. Useful `lx-*` helpers

The direct answer to "`dmesg -w` isn't enough": **`lx-dmesg`** reads the
ring buffer straight out of kernel memory, so it works *while VM B is
frozen at a breakpoint* — no live system required.

```gdb
(gdb) lx-dmesg                        # full ring buffer, offline, at this exact frozen instant
(gdb) lx-lsmod                        # confirm your module is loaded, refcount, dependents
(gdb) lx-ps                           # every task — find which process/kworker you're stopped in
(gdb) print *$lx_current()            # full task_struct of whatever's currently stopped
(gdb) lx-device-list-class("misc")    # walk the driver model - confirm a misc device registered
(gdb) lx-cpus                         # per-CPU state
(gdb) lx-version                       # confirm vmlinux matches VM B's running kernel
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

## Static verification, without any of the above

Every breakpoint target in the table above can be confirmed against
real compiled debug info with no VM B, no KGDB, and no root at all —
useful for checking a target resolves before committing to a live
session, or if you just want to see the DWARF info is really there:

```bash
cd NN_lab_name && make
gdb -q -batch -nx -ex "file module.ko" -ex "info line function_name" module.ko
```

`file module.ko` should report `with debug_info, not stripped`; `info
line` should report a real `file.c:LINE` and address, not an error.
