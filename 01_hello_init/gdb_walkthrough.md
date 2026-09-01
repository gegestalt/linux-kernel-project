# GDB walkthrough — 01_hello_init, hands-on, start to finish

`hello.c` is the smallest possible module: `init_module()` prints one
line and returns 0, `cleanup_module()` prints another. There's no bug to
find here — the point is building the *mechanical* muscle memory every
later module depends on: getting a breakpoint to fire at all inside code
that, before `insmod`, doesn't exist anywhere in kernel memory yet.

This module uses the legacy `init_module`/`cleanup_module` names
directly — no `module_init()`/`module_exit()` macros (see
[02_better_hello](../02_better_hello/) for the modern equivalent). That
matters here specifically: `init_module` is *also* the literal name of
the generic function every module's init funnels through
(`kernel/module/main.c`'s `do_init_module()` calls a function pointer it
names `init_module` internally) — so in this one session you'll run into
two different things both called `init_module` at different points.

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
cd /home/adiopocere/Desktop/codes/linux-kernel-project/01_hello_init
make -C /home/adiopocere/Desktop/codes/linux_mainline M=$(pwd) modules
```

## Step 2 — verify the source lines this module will stop on

```bash
gdb -q -batch -nx -ex "file hello.ko" -ex "info line init_module" -ex "info line cleanup_module" hello.ko
```
```
Line 9 of "hello.c" starts at address 0x18 <init_module> and ends at 0x20 <init_module+8>.
Line 20 of "hello.c" starts at address 0x60 <cleanup_module> and ends at 0x68 <cleanup_module+8>.
```

Line 9 is the `printk(KERN_INFO "Hello luv .\n");` call itself — the
function's opening brace (line 8) gets its own separate, zero-length
line entry. Normal DWARF/compiler behavior: GCC assigns the prologue to
the declaration line and the first real statement to the next one.

## Step 3 — check vermagic, copy onto the scratch disk

```bash
modinfo hello.ko | grep vermagic
```
```
vermagic: 7.2.0-kgdb-debug+ SMP preempt mod_unload modversions aarch64
```
```bash
sudo mount -o loop /home/adiopocere/Desktop/codes/qemu-vmb/labs-disk.img /tmp/vmb-mnt
sudo mkdir -p /tmp/vmb-mnt/01_hello_init
sudo cp hello.ko /tmp/vmb-mnt/01_hello_init/
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

## Step 5 — start gdb, connect, confirm the kernel matches

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

**What this shows:** `lx-version` should report `7.2.0-kgdb-debug+` —
matching `cat include/config/kernel.release` in this same tree. This is
the single most common KGDB mistake to skip and it fails *silently*
later otherwise: wrong line numbers, "optimized out" everywhere,
breakpoints that never fire, with no error pointing at the real cause.

## Step 6 — break on the generic module-init hook

`hello.ko` isn't loaded yet, so GDB has no symbol table entry for
anything inside it. `break init_module` right now would resolve to a
*different* function (the kernel's own generic one, not `hello.c`'s) or
fail outright — not what you want. Break on the function every module's
init funnels through instead, regardless of module name:

**Pane: gdb**

```
break do_init_module
```
```
continue
```

Prints `Continuing.` — switch panes.

## Step 7 — trigger the load

**Pane: vmb**

```bash
insmod /mnt/labs/01_hello_init/hello.ko
```

## Step 8 — first stop

**Pane: gdb**

```
Thread 2 hit Breakpoint 1, do_init_module (mod=mod@entry=0x...) at kernel/module/main.c:3089
3089    {
```

This is real, generic kernel code (`kernel/module/main.c`), not
`hello.c` — you're stopped *before* the kernel has even called into the
module. `mod` is a pointer to the `struct module` the kernel just built
for `hello.ko`:

```
print mod->name
```
```
$1 = "hello", '\000' <repeats ...>
```

## Step 9 — load the module's own symbols, break inside it

Now that `do_init_module` has been reached, the kernel has already
mapped `hello.ko`'s code into memory and published its section addresses
under `/sys/module/hello/sections/` — `lx-symbols` reads exactly that:

```
lx-symbols /home/adiopocere/Desktop/codes/linux-kernel-project
```
```
loading @0x...: /home/adiopocere/Desktop/codes/linux-kernel-project/01_hello_init/hello.ko
```

`hello.c`'s own two functions are now real, breakable symbols:

```
break init_module
```
```
Breakpoint 2 at 0x...: file hello.c, line 9.
```
```
continue
```

`insmod` is still sitting in `do_init_module`'s call chain from step 6's
`continue`, so it runs straight into this new breakpoint with no need to
touch the `vmb` pane again:

```
Thread 2 hit Breakpoint 2, init_module () at hello.c:9
9           printk(KERN_INFO "Hello luv .\n");
```

## Step 10 — step through init, confirm the return value

```
next
```
```
16          return 0;
```

Notice what `next` just skipped — the comment block (lines 12–15) isn't
code, so there's nothing there to stop on. `next` always moves
line-to-line in the *compiled* sense, not the textual one.

```
finish
```
```
Run till exit from #0  init_module () at hello.c:16
0xffffffffc0... in do_init_module (mod=0x...) at kernel/module/main.c:...
Value returned is $2 = 0
```

`finish` ran the rest of `init_module`, popped back into
`do_init_module` (the same generic function from step 8, now seen
*after* it called into your module), and shows the real return value:
`0`, exactly what the source returns. Cross-check against the kernel log
without detaching or needing the module to still be running —
`lx-dmesg` reads the ring buffer directly out of frozen kernel memory:

```
lx-dmesg
```
```
...
[   12.345678] hello: Hello luv .
```

## Step 11 — the exit path

```
break cleanup_module
```
```
continue
```

**Pane: vmb**

```bash
rmmod hello
```

**Pane: gdb**

```
Thread 2 hit Breakpoint 3, cleanup_module () at hello.c:20
20          printk(KERN_INFO "bye bye my luv\n");
```

`cleanup_module` already exists as a real symbol at this point — no
catch-all breakpoint needed the way `do_init_module` was for the load
path, since the module's own code is already mapped and symbolized.

```
bt
```

The mirror image of `do_init_module`'s call chain on the way in: a
`delete_module(2)` syscall entry chain down to this module's own
`cleanup_module` — the exact generic removal machinery `rmmod` is a thin
wrapper around. (Exact frame names depend on your kernel's syscall entry
naming convention — read whatever `bt` actually prints.)

```
next
```
```
21      }
```
```
finish
```
```
Run till exit from #0  cleanup_module () at hello.c:21
Value returned has type void.
```

`cleanup_module` returns `void` — `finish` reports "Value returned has
type void" rather than a numeric value, itself worth noticing: not every
`finish` gives you a value to inspect, and that's determined by the
function's actual signature, not by GDB.

## Step 12 — clean up

**Pane: gdb**

```
delete
```
```
y
```

**Pane: vmb**

```bash
poweroff -f
```

**Pane: gdb**

```
quit
```

---

## What this proves

- An out-of-tree module has no symbols until the kernel has actually
  mapped it into memory — which is why `do_init_module` (or, for modules
  built with the modern `module_init()` macro, see 02, a different
  generic hook) is always the first breakpoint, never the module's own
  init function directly (steps 6–9).
- `finish`'s reported return value isn't a guess or static analysis —
  it's GDB reading the actual register (or stack slot, depending on ABI)
  the real, running function just placed its return value into, at the
  exact instant it returned (step 10).
- `init_module` names two different things in this one session: the
  kernel's own generic function pointer name inside `do_init_module()`,
  and, only after `lx-symbols`, this module's own function. Every later
  module in this repo uses `module_init()`/`module_exit()` instead
  specifically to avoid this ambiguity — see 02's own walkthrough for
  exactly what that macro changes.
