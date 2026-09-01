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
modinfo ./better_hello.ko    # now shows author + description, unlike module 01
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

Two fully self-contained, hands-on walkthroughs for this module — tmux
session creation, build, boot, every gdb command, every expected output,
and cleanup, start to finish, no other file needed:

- [`gdb_walkthrough.md`](gdb_walkthrough.md) — the `module_init()`/
  `module_exit()` alias mechanism and the `__init`/`__exit` section
  relocation gotcha on unload.
- [`vermagic_debugging/insmod_vermagic_walkthrough.md`](vermagic_debugging/insmod_vermagic_walkthrough.md) —
  how `insmod` actually checks a module's `vermagic` against the running
  kernel, parameter by parameter, including a live demo of a module built
  against the wrong kernel getting rejected.

