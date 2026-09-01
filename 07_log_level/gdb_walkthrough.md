# GDB walkthrough — 07_log_level, hands-on, start to finish

`printk_log_levels.c` emits one message at every standard `printk`
priority (`KERN_EMERG` through `KERN_DEBUG`). There's no branching logic
and no shared state to inspect — this module's actual subject is
`dmesg`'s own filtering behavior, which lives in the *console* and *ring
buffer* layers, not in this driver's code. GDB's role here is narrower
and more precise than in other modules: proving, byte for byte, what
priority value each `pr_*()` helper actually attaches to its message,
and showing that `pr_debug()`'s appearance depends on kernel
configuration and dynamic-debug state that has nothing to do with this
module at all.

Every command below says exactly which pane. One command per step,
always — paste it, wait for the prompt to come back, then the next one.

---

## Step 0 — start the tmux session

*Regular terminal, not tmux yet.*

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

Two panes now: **vmb** (left) and **gdb** (right).

## Step 1 — build it

*Regular terminal (detach with `Ctrl-b d`, or a separate window).*

```bash
cd 07_log_level
make -C /home/adiopocere/Desktop/codes/linux_mainline M=$(pwd) modules
```

## Step 2 — confirm the real breakpoint target, statically

```bash
gdb -q -batch -nx -ex "file printk_log_levels.ko" \
    -ex "info line printk_log_levels_init" -ex "info line printk_emit_all_levels" \
    printk_log_levels.ko
```
```
Line 40 of "printk_log_levels.c" starts at address 0x3a8 <printk_log_levels_init> ...
Line 17 of "printk_log_levels.c" starts at address 0x3cc <printk_log_levels_init+36> ...
```

**There is no standalone `printk_emit_all_levels` symbol** — GCC inlined
its entire body directly into `printk_log_levels_init`, the same
compiler behavior module 11 hits with `increment_once()` and module 13
hits with `do_allocate()`. `break printk_log_levels_init` is the only
breakpoint this module needs; `next`-stepping through it walks straight
through what used to be a separate function call, no extra frame to
descend into.

## Step 3 — check vermagic, copy onto the scratch disk

```bash
modinfo printk_log_levels.ko | grep vermagic
```
```bash
sudo mount -o loop /home/adiopocere/Desktop/codes/qemu-vmb/labs-disk.img /tmp/vmb-mnt
sudo mkdir -p /tmp/vmb-mnt/07_log_level
sudo cp printk_log_levels.ko /tmp/vmb-mnt/07_log_level/
sudo umount /tmp/vmb-mnt
```

## Step 4 — boot the guest

**Pane: vmb**

```bash
qemu-system-aarch64 -M virt -cpu max -m 1024 -smp 2 \
  -kernel /home/adiopocere/Desktop/codes/linux_mainline/arch/arm64/boot/Image \
  -initrd /home/adiopocere/Desktop/codes/qemu-vmb/initramfs.cpio.gz \
  -drive file=/home/adiopocere/Desktop/codes/qemu-vmb/labs-disk.img,if=virtio,format=raw \
  -append "console=ttyAMA0 rdinit=/init nokaslr" -nographic -s
```

Wait for `=== VM B (QEMU) ready ===` and `~ #`.

## Step 5 — start gdb, connect

**Pane: gdb**

```bash
cd /home/adiopocere/Desktop/codes/linux_mainline && gdb -q -iex 'set auto-load safe-path /' vmlinux
```
```
target remote :1234
```
```
lx-version
```

Breaking on a not-yet-loaded module's function — before `insmod` — never
works regardless of inlining, the symbol simply doesn't exist yet.
Bootstrap via `do_init_module` first, same as every module in this repo:

```
break do_init_module
```
```
continue
```

Prints `Continuing.` — switch panes.

## Step 6 — trigger the load

**Pane: vmb**

```bash
insmod /mnt/labs/07_log_level/printk_log_levels.ko
```

## Step 7 — load symbols, break on the real (inlined-into) function

**Pane: gdb**

```
lx-symbols /home/adiopocere/Desktop/codes/linux-kernel-project
```
```
break printk_log_levels_init
```
```
continue
```
```
Thread 2 hit Breakpoint N, printk_log_levels_init () at printk_log_levels.c:40
```
```
next
```
```
lx-dmesg
```

`lx-dmesg` reads the ring buffer straight out of kernel memory — it
works even with the guest frozen mid-breakpoint, unlike `dmesg` from a
`vmb` shell.

```
next
```
```
next
```
```
lx-dmesg
```

## Step 8 — read the eight calls' worth of assembly directly

Rather than stepping all eight individually, disassemble the region once
to see exactly what each `pr_*()` macro expanded into — the most direct
way to answer "what number does `pr_warn` actually use":

```
disassemble printk_log_levels_init
```

Look for the calls into `printk`/`vprintk_emit`/`_printk` (exact symbol
depends on this kernel's printk implementation — `_printk` on most
modern trees) and, immediately before each, a format-string pointer
being loaded — each one literally embeds a one-byte priority prefix
(`'\001'` followed by the digit `'0'`–`'7'`) at the front of its string
constant. Confirm this directly rather than trust the macro definitions
— stop at any one call site and read the format-string pointer's target:

```
x/s $rdi
```

(Or whichever register/argument holds the fmt pointer at the call site
you stopped on — depends on where in the disassembly you are.) You'll
see something like
`"\001\006printk_log_levels: level 6: KERN_INFO demonstration\n"` — the
`\001\006` is the encoded `KERN_INFO` (numeric level 6) prefix
`pr_info()` attached, completely invisible in the terminal's normal
`dmesg` rendering because the console driver strips it before display.
This is the actual mechanism behind every `pr_*()`/`KERN_*` level in the
entire kernel, made visible as raw bytes instead of taken on faith.

## Step 9 — `pr_debug()` depends on configuration, not this module

```
finish
```

**Pane: vmb**

```bash
dmesg | grep -c "level 7"
```

Compare this count against a naive reading of
`printk_emit_all_levels()`'s source — it calls both `pr_debug()` *and*
an explicit `printk(KERN_DEBUG ...)` for level 7, so you'd expect two
"level 7" lines. This debug kernel has `CONFIG_DYNAMIC_DEBUG=y`
(confirmed: `grep CONFIG_DYNAMIC_DEBUG
/home/adiopocere/Desktop/codes/linux_mainline/.config` on the host — the
guest's minimal busybox rootfs has no `/boot` to check from inside),
which changes what `pr_debug()` actually compiles to: not a no-op, but a
real, individually-toggleable call site that's *disabled by default*
until something enables it through
`/sys/kernel/debug/dynamic_debug/control`. So you should see exactly
**one** "level 7" line right now (the explicit `printk(KERN_DEBUG ...)`),
not two — `pr_debug()`'s own message is silently suppressed, not absent
from the binary.

Confirm the call site exists and flip it live, no reload required:

```bash
cat /sys/kernel/debug/dynamic_debug/control | grep printk_log_levels
```
```bash
echo "module printk_log_levels +p" > /sys/kernel/debug/dynamic_debug/control
```
```bash
rmmod printk_log_levels
```
```bash
insmod /mnt/labs/07_log_level/printk_log_levels.ko
```
```bash
dmesg | tail -3
```

`pr_debug()`'s line now appears where it didn't before — the *exact same
compiled `.ko`*, the only thing that changed is a runtime dynamic-debug
flag, no rebuild.

## Step 10 — clean up

No breakpoints are actually armed at this point — `finish` in step 9
already ran to completion. `printk_log_levels_exit()` is a single
`pr_info()` call with nothing else to inspect, so there's no reason to
break on it — unlike every other module past 01, whose `__exit`-marked
cleanup function would need the `.exit.text` relocation workaround
(covered in module 02's walkthrough) to catch at all.

**Pane: vmb**

```bash
rmmod printk_log_levels
```
```bash
poweroff -f
```

**Pane: gdb**

```
quit
```

---

## What this proves

`pr_*()`/`KERN_*` priority levels are literal encoded bytes prepended to
the format string, not metadata attached some other way — reading them
straight out of memory with `x/s` at the exact call site (step 8) is
strictly more convincing than any documentation. And `pr_debug()`'s
behavior genuinely depends on kernel build configuration and runtime
dynamic-debug state that live entirely outside this module's own code
(step 9) — the same eight lines of source can visibly produce different
`dmesg` output on two different kernels, or even on the same kernel
before and after a `dynamic_debug/control` write, with nothing about the
module itself having changed.
