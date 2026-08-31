# linux-kernel-project

Hands-on Linux kernel module development, labs 01 through 16, each one
isolating a new concept on top of the ones before it. Go through them in
order the first time; after that it's a reference you jump back into.

## Where to find things

| Looking for... | Go to |
|---|---|
| What a specific lab does, how to build/run it | `NN_lab_name/README.md` |
| A live GDB session walking through that lab's code | `NN_lab_name/GDB_WALKTHROUGH.md` |
| Every lab's run commands in one place, no explanations | [`RUNBOOK.md`](RUNBOOK.md) |
| One-time setup for the GDB/QEMU debugging environment | [`GDB_DEBUGGING.md`](GDB_DEBUGGING.md) |
| Getting a module onto a real machine — DKMS, signing, upstreaming | [`DEPLOYMENT.md`](DEPLOYMENT.md) |

Each lab directory is self-contained: its own `Makefile` and `.c` file(s),
built independently with `make`. Nothing is shared between labs except the
vendored kernel source at [`../linux_mainline`](../linux_mainline), used for
`checkpatch.pl` linting and as a source reference.

```
NN_name/
├── README.md            # what this lab does, how to run it
├── GDB_WALKTHROUGH.md    # step-by-step GDB session for this lab
├── Makefile              # make / make clean / make check
├── *.c                   # the module (and, in some labs, a userspace test helper)
└── (build output)        # *.ko, *.o, etc. — gitignored
```

## Build and run

```bash
uname -r
ls /lib/modules/$(uname -r)/build   # must exist — install linux-headers-$(uname -r) if not
```

```bash
cd NN_lab_name
make                        # build the .ko
sudo insmod ./module.ko     # load
dmesg -w                    # watch kernel log, in another shell
# interact with the module: sysfs, /dev, /proc, ioctl...
sudo rmmod module_name      # unload
make clean
```

Each lab's own `README.md` has the exact device names, params, and sysfs
paths for that lab.
