# linux-kernel-project

Hands-on Linux kernel module development, modules 01 through 17, each one
isolating a new concept on top of the ones before it. Go through them in
order the first time; after that it's a reference you jump back into.
Module 17 is the exception to the strict ordering — it's a deliberate
return to module 04's topic (module parameters) once there's more
context to make a genuinely complex example land.

## Where to find things

| Looking for... | Go to |
|---|---|
| What a specific module does, how to build/run it | `NN_module_name/readme.md` |
| A live GDB session walking through that module's code, start to finish — tmux, build, boot, breakpoints, cleanup | `NN_module_name/gdb_walkthrough.md` |
| Every module's run commands in one place, no explanations | [`runbook.md`](runbook.md) |
| Getting a module onto a real machine — DKMS, signing, upstreaming | [`deployment.md`](deployment.md) |

Each module's directory is self-contained: its own `Makefile` and `.c`
file(s), built independently with `make`. Nothing is shared between them
except the vendored kernel source at
[`../linux_mainline`](../linux_mainline), used for `checkpatch.pl` linting
and as a source reference.

```
NN_name/
├── readme.md            # what this module does, how to run it
├── gdb_walkthrough.md    # step-by-step GDB session for this module
├── Makefile              # make / make clean / make check
├── *.c                   # the module's source (and, in some cases, a userspace test helper)
└── (build output)        # *.ko, *.o, etc. — gitignored
```

## Build and run

```bash
uname -r
ls /lib/modules/$(uname -r)/build   # must exist — install linux-headers-$(uname -r) if not
```

```bash
cd NN_module_name
make                        # build the .ko
sudo insmod ./module.ko     # load
dmesg -w                    # watch kernel log, in another shell
# interact with the module: sysfs, /dev, /proc, ioctl...
sudo rmmod module_name      # unload
make clean
```

Each module's own `readme.md` has the exact device names, params, and sysfs
paths for it.
