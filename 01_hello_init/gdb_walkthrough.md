# GDB walkthrough — 01_hello_init

`hello.c` is the smallest possible module: `init_module()` prints one
line and returns 0, `cleanup_module()` prints another line. There is no
logic to get wrong here — the point of this walkthrough isn't to find a
bug, it's to build the *mechanical* muscle memory every later module
depends on: getting a breakpoint to fire at all inside code that,
before `insmod`, doesn't exist anywhere in kernel memory yet. Every
later module's walkthrough assumes you've done this one first.

Note the function names: this module still uses the legacy
`init_module`/`cleanup_module` symbols directly (no `module_init()`/
`module_exit()` macros, no `__init`/`__exit` — see 02_better_hello for
the modern equivalent). That's not an accident to work around; it's
exactly why module 01 is the right place to learn the "module isn't
loaded yet" problem, because `init_module` is also the literal name of
the generic kernel function every module's init funnels through
(`kernel/module/main.c`'s `do_init_module()` calls a function pointer
it names `init_module` internally) — so for this one module, and only
this one, you'll see two different `init_module`s at different points
in the same session.

## Environment

One-time QEMU + busybox-initramfs setup: [`../gdb_debugging.md`](../gdb_debugging.md#qemu-path-recommended-the-debug-kernel-right-here-no-second-vm).
Build this module against the debug tree, not your host kernel:

```bash
cd 01_hello_init
make -C /home/adiopocere/Desktop/codes/linux_mainline M=$(pwd) modules
modinfo hello.ko | grep vermagic   # must read 7.2.0-kgdb-debug+
```

Copy it onto the scratch disk:

```bash
sudo mount -o loop /home/adiopocere/Desktop/codes/qemu-vmb/labs-disk.img /tmp/vmb-mnt
sudo mkdir -p /tmp/vmb-mnt/01_hello_init
sudo cp hello.ko /tmp/vmb-mnt/01_hello_init/
sudo umount /tmp/vmb-mnt
```

## tmux layout

Two tmux sessions, per the main guide: `vmb` (the QEMU guest's serial
console) and `gdbsess` (GDB, running on this machine, never freezes).
Boot the guest and start GDB exactly as shown in
[`../gdb_debugging.md`](../gdb_debugging.md#boot-the-guest-one-tmux-pane-and-attach-gdb-another) —
including `nokaslr` on the kernel command line and `gdb -q -iex 'set
auto-load safe-path /' vmlinux`. Attach to each with `tmux attach -t
vmb` / `tmux attach -t gdbsess` in two terminals (or two tmux clients),
and switch between them — don't type gdb commands into the vmb pane or
vice versa.

## The walkthrough

Connect GDB to the guest and confirm the kernel matches before doing
anything else — this is the single most common KGDB mistake and it
fails silently otherwise (wrong line numbers, "optimized out"
everywhere, breakpoints that never fire):

```gdb
(gdb) target remote :1234
(gdb) lx-version
```

`lx-version` should report `7.2.0-kgdb-debug+`, matching `cat
/home/adiopocere/Desktop/codes/linux_mainline/include/config/kernel.release`.

### Step 1 — break on the generic module-init hook

`hello.ko` isn't loaded yet, so GDB has no symbol table entry for
anything inside it — `break init_module` right now would actually
resolve to a *different* function (see the note above) or fail
outright, not to `hello.c`'s `init_module`. Instead, break on the one
function every module's init funnels through regardless of what the
module itself is called:

```gdb
(gdb) break do_init_module
(gdb) continue
```

In the `vmb` pane:

```bash
insmod /mnt/labs/01_hello_init/hello.ko
```

GDB stops:

```
Thread 2 hit Breakpoint 1, do_init_module (mod=mod@entry=0x...) at kernel/module/main.c:3089
3089    {
```

This is real, generic kernel code (`kernel/module/main.c`), not
`hello.c` — you're stopped *before* the kernel has even called into the
module. `mod` is a pointer to the `struct module` the kernel just
built for `hello.ko`; `print mod->name` here will show `"hello"`.

### Step 2 — load the module's own symbols

Now that `do_init_module` has been reached, the kernel has already
mapped `hello.ko`'s code into memory and published its section
addresses under `/sys/module/hello/sections/` — `lx-symbols` reads
exactly that:

```gdb
(gdb) lx-symbols /home/adiopocere/Desktop/codes/linux-kernel-project
loading @0x...: /home/adiopocere/Desktop/codes/linux-kernel-project/01_hello_init/hello.ko
```

From this point on, `hello.c`'s own two functions are real, breakable
symbols — confirmed statically against the built `.ko` (no VM needed
for this part):

```
$ gdb -q -batch -nx -ex "file hello.ko" -ex "info line init_module" -ex "info line cleanup_module" hello.ko
Line 9 of "hello.c" starts at address 0x18 <init_module> and ends at 0x20 <init_module+8>.
Line 20 of "hello.c" starts at address 0x60 <cleanup_module> and ends at 0x68 <cleanup_module+8>.
```

Line 9 is the `printk(KERN_INFO "Hello luv .\n");` call itself (the
function's opening brace, line 8, gets its own separate, zero-length
line entry — this is a normal DWARF/compiler artifact, not a bug: GCC
often assigns the prologue to the declaration line and the first real
statement to the next).

### Step 3 — break inside the module itself and step through init

```gdb
(gdb) break init_module
Breakpoint 2 at 0x...: file hello.c, line 9.
(gdb) continue
```

`insmod` is still sitting in `do_init_module`'s call chain from step
1's `continue`, so it runs straight into the new breakpoint without
you touching the `vmb` pane again:

```
Thread 2 hit Breakpoint 2, init_module () at hello.c:9
9           printk(KERN_INFO "Hello luv .\n");
(gdb) next
16          return 0;
```

Notice what `next` just skipped: the comment block (lines 12–15) isn't
code, so the debugger has nothing to stop on there — `next` always
moves line-to-line in the *compiled* sense, not the textual one.

```gdb
(gdb) finish
Run till exit from #0  init_module () at hello.c:16
0xffffffffc0... in do_init_module (mod=0x...) at kernel/module/main.c:...
Value returned is $1 = 0
```

`finish` runs the rest of `init_module`, pops back out into
`do_init_module` (the same generic function from step 1 — you're now
seeing it *after* it called into your module), and shows you the
return value: `0`, exactly what the source returns, confirming
`insmod` will report success. Cross-check against dmesg without
needing the module to still be running or GDB to detach — `lx-dmesg`
reads the ring buffer directly out of frozen kernel memory:

```gdb
(gdb) lx-dmesg
...
[   12.345678] hello: Hello luv .
```

### Step 4 — the exit path

```gdb
(gdb) break cleanup_module
(gdb) continue
```

In `vmb`:

```bash
rmmod hello
```

Back in `gdbsess`:

```
Thread 2 hit Breakpoint 3, cleanup_module () at hello.c:20
20          printk(KERN_INFO "bye bye my luv\n");
(gdb) next
21      }
(gdb) finish
```

`cleanup_module` returns `void` — `finish` will report "Value returned
has type void." rather than a numeric value, which is itself worth
noticing: not every `finish` gives you a return value to inspect, and
that's determined by the function's actual signature, not by GDB.

## Cleanup

```gdb
(gdb) delete 1 2 3
```

(Bare `delete` with no arguments deletes *all* breakpoints too, but it
first asks `Delete all breakpoints? (y or n)` — if you're driving this
session non-interactively (piping commands in, or typing ahead) that
confirmation prompt can eat your next command instead of actually
deleting anything, leaving stale breakpoints active. Naming the numbers
explicitly, as above, skips the prompt entirely and is the safer habit
to build now, before it costs you a confusing session later.)

```bash
# in vmb:
poweroff -f
```
```bash
# in gdbsess, or a fresh shell:
tmux kill-session -t vmb
tmux kill-session -t gdbsess
```

## What this proves

Two things that generalize to every other module in this repo: first, an
out-of-tree module has no symbols until the kernel has actually mapped
it into memory, which is why `do_init_module` (or, for modules built
with the modern `module_init()` macro — see 02 — a different generic
hook) is always the first breakpoint, never the module's own init
function. Second, `finish`'s reported return value is not a guess or a
static analysis — it is GDB reading the actual register (or stack
slot, depending on ABI) the real, running function just placed its
return value into, at the exact instant it returned. Everything after
this module builds on exactly this mechanism, aimed at less trivial code.
