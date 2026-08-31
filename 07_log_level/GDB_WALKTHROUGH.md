# GDB walkthrough — 07_log_level

`printk_log_levels.c` emits one message at every standard `printk`
priority (`KERN_EMERG` through `KERN_DEBUG`). There's no branching
logic and no shared state to inspect — this lab's actual subject is
`dmesg`'s own filtering behavior, which lives in the *console* and
*ring buffer* layers, not in this driver's code. GDB's role here is
narrower and more precise than in other labs: proving, byte for byte,
what priority value each `pr_*()` helper actually attaches to its
message, and showing that `pr_debug()`'s appearance depends on kernel
configuration and dynamic-debug state that has nothing to do with this
module at all.

## Environment

```bash
cd 07_log_level
make -C /home/adiopocere/Desktop/codes/linux_mainline M=$(pwd) modules
modinfo printk_log_levels.ko | grep vermagic
sudo mount -o loop /home/adiopocere/Desktop/codes/qemu-vmb/labs-disk.img /tmp/vmb-mnt
sudo mkdir -p /tmp/vmb-mnt/07_log_level
sudo cp printk_log_levels.ko /tmp/vmb-mnt/07_log_level/
sudo umount /tmp/vmb-mnt
```

## tmux layout

Standard `vmb` + `gdbsess` — [`../GDB_DEBUGGING.md`](../GDB_DEBUGGING.md).

## The walkthrough

### Step 1 — `printk_emit_all_levels` is inlined; find where it really lives

```gdb
(gdb) target remote :1234
(gdb) lx-version
(gdb) break do_init_module
(gdb) continue
```
```bash
# vmb:
insmod /mnt/labs/07_log_level/printk_log_levels.ko
```
```gdb
(gdb) lx-symbols /home/adiopocere/Desktop/codes/linux-kernel-project
```

Try to break on the function that actually contains the eight
`pr_*()` calls:

```gdb
(gdb) info line printk_emit_all_levels
Line 17 of "printk_log_levels.c" starts at address 0x3cc <printk_log_levels_init+36> and ends at 0x3d8 <printk_log_levels_init+48>.
```

**There is no standalone `printk_emit_all_levels` symbol** — GCC
inlined its entire body directly into `printk_log_levels_init`, the
same compiler behavior lab 11 hits with `increment_once()` and lab 13
hits with `do_allocate()`. `break printk_log_levels_init` is therefore
the only breakpoint this lab needs; `next`-stepping through it walks
straight through what used to be a separate function call with no
extra frame to descend into.

```gdb
(gdb) break printk_log_levels_init
(gdb) continue
Thread 2 hit Breakpoint N, printk_log_levels_init () at printk_log_levels.c:40
(gdb) next
```

### Step 2 — step through each priority level, one instruction at a time

```gdb
(gdb) step    # into pr_emerg's expansion - actually just next, it's one printk call
```

Rather than stepping all eight individually, `disassemble` the region
once to see exactly what each `pr_*()` macro expanded into — this is
the most direct way to answer "what number does `pr_warn` actually
use":

```gdb
(gdb) disassemble printk_log_levels_init
```

Look for the calls into `printk`/`vprintk_emit`/`_printk` in the
disassembly (exact symbol depends on this kernel's printk
implementation — `_printk` on most modern trees) and, immediately
before each, a format-string pointer being loaded — each one
literally embeds a one-byte priority prefix (`'\001'` followed by the
digit `'0'`–`'7'`) at the front of its string constant. Confirm this
directly rather than trust the macro definitions:

```gdb
(gdb) x/s $rdi     # or whichever register/arg holds the fmt pointer at a given pr_*() call site
```

Depending on where you stop, you'll see something like
`"\001\006printk_log_levels: level 6: KERN_INFO demonstration\n"` — the
`\001\006` is the encoded `KERN_INFO` (numeric level 6) prefix
`pr_info()` attached, completely invisible in the terminal's normal
`dmesg` rendering because the console driver strips it before display.
This is the actual mechanism behind every `pr_*()`/`KERN_*` level in
the entire kernel, made visible as raw bytes instead of taken on faith.

### Step 3 — `pr_debug()` depends on configuration, not this module

```gdb
(gdb) finish
```
```bash
# vmb:
dmesg | grep -c "level 7"
```

Compare this count against what you'd expect from a plain reading of
`printk_emit_all_levels()`'s source — it calls both `pr_debug()`
*and* an explicit `printk(KERN_DEBUG ...)` for level 7, so you'd
naively expect two "level 7" lines. This debug kernel has
`CONFIG_DYNAMIC_DEBUG=y` (confirmed:
`grep CONFIG_DYNAMIC_DEBUG /home/adiopocere/Desktop/codes/linux_mainline/.config`
on the host — the guest's minimal busybox rootfs has no `/boot` to check
from inside), which changes what `pr_debug()` actually compiles to: not
a no-op, but a real, individually-toggleable call site that's *disabled
by default* until something enables it through
`/sys/kernel/debug/dynamic_debug/control`. So you should see exactly
**one** "level 7" line right now (the explicit `printk(KERN_DEBUG
...)`), not two — `pr_debug()`'s own message is silently suppressed,
not absent from the binary. Confirm the call site exists and flip it
live, no reload required:

```bash
# vmb:
cat /sys/kernel/debug/dynamic_debug/control | grep printk_log_levels
echo "module printk_log_levels +p" > /sys/kernel/debug/dynamic_debug/control
```

Reload the module (`rmmod`/`insmod` again) with that flag now enabled
and confirm `pr_debug()`'s line finally appears in `dmesg` where it
didn't before — the *exact same compiled `.ko`*, the only thing that
changed is a runtime dynamic-debug flag, no rebuild.

## Cleanup

```gdb
(gdb) delete
```
```bash
# vmb:
rmmod printk_log_levels
poweroff -f
```

## What this proves

`pr_*()`/`KERN_*` priority levels are literal encoded bytes prepended
to the format string, not metadata attached some other way — reading
them straight out of memory with `x/s` at the exact call site is
strictly more convincing than any documentation. And `pr_debug()`'s
behavior genuinely depends on kernel build configuration and runtime
dynamic-debug state that live entirely outside this module's own code
— the same eight lines of source can visibly produce different `dmesg`
output on two different kernels, or even on the same kernel before and
after a `dynamic_debug/control` write, with nothing about the module
itself having changed.
