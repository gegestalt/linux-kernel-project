# 02 — better_hello

The same module as [01](../01_hello_init/), rewritten with the interfaces
the kernel actually wants a module to use.

## What this demonstrates

- `module_init()` / `module_exit()` — macros that register your init/exit
  functions by pointer rather than by fixed name. This is what lets a
  module name its functions anything (`my_init`, `my_exit` here), and it's
  the mechanism `01_hello_init`'s `init_module`/`cleanup_module` predates.
- `__init` / `__exit` section attributes. `__init` puts a function in the
  `.init.text` section, which the kernel **frees after the module finishes
  loading** for built-in code (and, for loadable modules like these, marks
  as discardable) — code there must never be called again after `init`
  returns. `__exit` is the mirror case: if the module is built statically
  into the kernel (`CONFIG_MODULE_UNLOAD=n` or built-in), the exit path is
  dead code and the compiler is told it can discard it entirely.
- `MODULE_AUTHOR()` / `MODULE_DESCRIPTION()` — metadata that `modinfo`
  reads directly out of the `.modinfo` ELF section, no runtime cost.

## Files

| File | Purpose |
|---|---|
| `better_hello.c` | The module: `my_init()`/`my_exit()` registered via the macros. |
| `Makefile` | Out-of-tree Kbuild wrapper (uses `KDIR`/`PWD` variables — compare with 01's inline version). |

## Build

```bash
cd 02_better_hello
make
```

## Load and test

```bash
sudo insmod ./better_hello.ko
dmesg | tail -3
modinfo ./better_hello.ko    # now shows author + description, unlike lab 01
sudo rmmod better_hello
dmesg | tail -3
```

## Cleanup

```bash
make clean
```

## Things to try

- `objdump -h better_hello.ko | grep -E 'init|exit'` — find the `.init.text`
  and `.exit.text` sections and confirm `my_init`/`my_exit` actually land
  there (`nm better_hello.ko | grep my_init`, then cross-reference the
  address range against `objdump`'s section table).
- Delete the `__init`/`__exit` attributes and rebuild. The module still
  works identically as a loadable module — the attributes are an
  optimization/documentation hint here, not a correctness requirement for
  `.ko` files (they matter much more for code built into `vmlinux`).
- Compare `hello.c` and `better_hello.c` line by line. Everything that
  changed is the entry-point mechanism and the metadata; the `printk()`
  payload is identical.

## Debugging with GDB

Setup: [`../GDB_DEBUGGING.md`](../GDB_DEBUGGING.md). Both targets
confirmed to resolve before being written down:

```bash
$ gdb -q -batch -nx -ex "file better_hello.ko" -ex "info line my_init" -ex "info line my_exit" better_hello.ko
Line 9 of "better_hello.c" starts at address 0x38 <my_init> and ends at 0x40 <my_init+8>.
Line 21 of "better_hello.c" starts at address 0x78 <my_exit> and ends at 0x80 <my_exit+8>.
```

**The load path.**

```gdb
(gdb) break do_init_module
(gdb) continue
```
```bash
sudo insmod ./better_hello.ko
```
```gdb
(gdb) lx-symbols /home/adiopocere/Desktop/codes/linux-kernel-project
(gdb) break my_init          # not init_module - this lab uses module_init()'s indirection
(gdb) continue
```

The interesting difference from lab 01: `break init_module` here would
fail to resolve (no such symbol) — `module_init(my_init)` registers
`my_init` through a pointer in the `.initcall` mechanism rather than
exporting a fixed name, so you have to break on the *actual* function
name this time. `info symbol my_init` after the break confirms which
section (`.init.text`) it's sitting in, matching this lab's own
`objdump -h` exercise above.

**The unload path.** Same story as lab 01's `cleanup_module`, but with a
twist worth checking for yourself: `my_exit` is marked `__exit`, which
this lab's README explains gets discarded entirely for code built
statically into `vmlinux` — but for a *loadable* module like this one,
the function is very much still there and callable:

```gdb
(gdb) break my_exit
(gdb) continue
```
```bash
sudo rmmod better_hello
```
```gdb
(gdb) next
(gdb) finish
```

If you're curious exactly how `__exit`/`__init` are implemented rather
than taking the README's word for it: `info address my_exit` here vs.
`info address my_init` from the load path — both report addresses, both
inside this module's mapped text, because `.ko` files don't discard
either section the way a built-in `vmlinux` would. Cross-reference
against `objdump -h better_hello.ko` from this lab's own "Things to try"
to see which named section (`.init.text` vs `.exit.text`) each address
actually falls in.

