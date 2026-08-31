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
  refuses you a long list of GPL-only symbols. Every later lab keeps this.
- The insmod/rmmod lifecycle: a `.ko` file is inert until `insmod` maps it
  into kernel address space and calls your init function; it stays resident
  until `rmmod` calls your exit function and the kernel unmaps it.

Lab [02](../02_better_hello/) rewrites this exact module using the interface
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
module doesn't set those. Lab 02 does.

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

Full environment setup (debug kernel build, KGDB-over-serial, `lx-symbols`)
is in [`../GDB_DEBUGGING.md`](../GDB_DEBUGGING.md). Once attached:

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
directly (see what this lab demonstrates, above), those are literally the
symbol names GDB resolves — no macro indirection to see through, which
makes this the simplest possible first GDB session before trying a lab
with real state to inspect.

