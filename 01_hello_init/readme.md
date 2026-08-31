# 01 — hello_init

The smallest thing that can legally be called a Linux kernel module: two
functions and a license tag.

## What this demonstrates

- The **legacy** module entry points, `init_module()` and `cleanup_module()`.
  Every kernel module needs *some* function the kernel calls on load and
  unload; before the `module_init()`/`module_exit()` macros existed (and
  still today, if you name your functions exactly `init_module`/
  `cleanup_module`), the kernel finds them by that fixed name.
- `printk()` — the only way to get a message out of kernel space that isn't
  a crash. There's no `stdout` in the kernel.
- `MODULE_LICENSE("GPL")` — without it, the kernel taints itself on load and
  refuses you a long list of GPL-only symbols. Every module after this one
  keeps this.
- The insmod/rmmod lifecycle: a `.ko` file is inert until `insmod` maps it
  into kernel address space and calls your init function; it stays resident
  until `rmmod` calls your exit function and the kernel unmaps it.

Module [02](../02_better_hello/) rewrites this exact module using the interface
the kernel actually wants you to use today (`module_init()`/`module_exit()`),
so you can compare `hello.c` and `better_hello.c` side by side and see
exactly what the macros buy you.

## Files

| File | Purpose |
|---|---|
| `hello.c` | The module: `init_module()`, `cleanup_module()`. |
| `Makefile` | Out-of-tree Kbuild wrapper. |

## Build

```bash
cd 01_hello_init
make
```

This invokes the running kernel's build system against this directory
(`M=$(PWD)`) and produces `hello.ko`. You need `linux-headers-$(uname -r)`
installed — `ls /lib/modules/$(uname -r)/build` should exist.

## Load and test

```bash
sudo insmod ./hello.ko
dmesg | tail -5          # expect: "Hello luv ."
lsmod | grep hello        # confirm it's resident
sudo rmmod hello
dmesg | tail -5          # expect: "bye bye my luv"
```

`modinfo hello.ko` will show the license but no author/description — this
module doesn't set those. Module 02 does.

## Cleanup

```bash
make clean
```

## Things to try

- Change `init_module()` to `return -EINVAL;` instead of `0`. Rebuild,
  `insmod` it, and read the exit status (`echo $?`) plus `dmesg` — a
  non-zero return means the module never actually loads, and
  `cleanup_module()` is *not* called.
- Rename `init_module`/`cleanup_module` to something else and try to
  `insmod` — the module loads with no init/exit calls, since the kernel is
  matching those functions by exact symbol name, not by a registered
  callback.

## Debugging with GDB

For a full, self-contained, step-by-step session for this module — tmux
pane layout, every command, every output explained — see
[`gdb_walkthrough.md`](gdb_walkthrough.md). Full environment setup (debug kernel build, KGDB-over-serial, `lx-symbols`)
is in [`../gdb_debugging.md`](../gdb_debugging.md). Both breakpoint
targets below were confirmed to resolve to real, compiled debug info
before being written down here:

```bash
$ gdb -q -batch -nx -ex "file hello.ko" -ex "info line init_module" -ex "info line cleanup_module" hello.ko
Line 9 of "hello.c" starts at address 0x18 <init_module> and ends at 0x20 <init_module+8>.
Line 20 of "hello.c" starts at address 0x60 <cleanup_module> and ends at 0x68 <cleanup_module+8>.
```

**The load path.** `init_module` doesn't exist as a symbol until this
`.ko` is actually loading, so arm a catch-all first:

```gdb
(gdb) break do_init_module
(gdb) continue
```
```bash
sudo insmod ./hello.ko
```
```gdb
(gdb) lx-symbols /home/adiopocere/Desktop/codes/linux-kernel-project
(gdb) break init_module
(gdb) continue
(gdb) next                 # step onto the printk() line itself
(gdb) finish                # run to return, confirm the return value is 0
```

Because this module uses the legacy `init_module`/`cleanup_module` names
directly (see what this demonstrates, above), those are literally the
symbol names GDB resolves — no macro indirection to see through, which
makes this the simplest possible first GDB session before trying a module
with real state to inspect.

**The unload path.** Once the module is loaded, `cleanup_module` already
exists as a real symbol — no catch-all needed this time, just break on
it directly and unload from the guest:

```gdb
(gdb) break cleanup_module
(gdb) continue
```
```bash
sudo rmmod hello
```
```gdb
(gdb) bt                    # the call chain: sys_delete_module -> ... -> cleanup_module
(gdb) next                   # onto the second printk()
(gdb) finish                  # run to return - cleanup_module is void, nothing to inspect on return
```

`bt` here is worth reading closely: it's the mirror image of what
`do_init_module` showed you on the way in — a `delete_module(2)` syscall
entry chain down to this module's own `cleanup_module`, the exact
generic removal machinery `rmmod` is a thin wrapper around (the precise
frame names depend on your kernel version's syscall entry naming
convention — read whatever `bt` actually prints rather than expecting
an exact match here). Compare it against `bt` from the `init_module`
breakpoint above — same shape, opposite direction.

