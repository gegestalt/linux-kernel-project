# 10 — ioctl_basics

Custom device control operations, beyond what `read()`/`write()` can
express: the `_IO`/`_IOR`/`_IOW`/`_IOWR` macro family and a real
`unlocked_ioctl()` implementation, driven end-to-end by a userspace test
program built from the **same shared header** as the module.

## What this demonstrates

- All four ioctl command shapes, one of each, defined once in
  `ioctl_basics.h` and used by both `ioctl_basics.c` and `ioctl_test.c`:
  - `_IO(type, nr)` — `IOCTL_BASICS_RESET`: no payload, purely a signal.
  - `_IOR(type, nr, struct ioctl_basics_stats)` — `IOCTL_BASICS_GET_STATS`:
    kernel → userspace only.
  - `_IOW(type, nr, __u32)` — `IOCTL_BASICS_SET_MODE`: userspace → kernel
    only.
  - `_IOWR(type, nr, struct ioctl_basics_echo)` — `IOCTL_BASICS_ECHO`: both
    directions through the *same* buffer (userspace sends text, the driver
    transforms it in place, userspace reads the transformed result back).
- That the `datatype` argument to `_IOR`/`_IOW`/`_IOWR` is **not** passed to
  the kernel at call time — it only gets baked into the encoded command
  number (readable via `_IOC_SIZE(cmd)`), so tools like `strace` can print
  a plausible size without understanding this driver's actual semantics.
  The kernel side still has to `copy_to_user()`/`copy_from_user()` by hand,
  exactly like `read()`/`write()` do.
- `-ENOTTY` as the conventional "unrecognized ioctl command" error — not
  `-EINVAL`, which this driver reserves for a *recognized* command with an
  invalid argument (`SET_MODE` with an out-of-range mode value). The
  userspace test exercises both, deliberately, so you can see them differ.
- A single shared header as the entire contract between kernel and
  userspace code — the normal shape of a real ioctl-based interface (see
  e.g. `include/uapi/linux/*.h` in `../../linux_mainline` for how this
  scales to in-tree drivers).

## Files

| File | Purpose |
|---|---|
| `ioctl_basics.h` | Shared command numbers and payload structs. |
| `ioctl_basics.c` | The driver: `unlocked_ioctl()` implementing all four commands. |
| `ioctl_test.c` | Userspace program exercising every command, including the two expected-failure cases. |
| `Makefile` | Builds the module (`module`) and `ioctl_test`; `check`/`checkpatch` lints the kernel side (`.c` + `.h`) and compiles the userspace side with `-Wall -Wextra -Wpedantic -Werror`. |

## Build

```bash
cd 10_ioctl_basics
make            # builds ioctl_basics.ko and ioctl_test
```

## Load and test

```bash
sudo insmod ./ioctl_basics.ko
dmesg | tail -3
ls -l /dev/ioctl_basics0

sudo ./ioctl_test /dev/ioctl_basics0
```

Expected shape of the output: a reset, a stats read showing zeroed
counters, three echoes (identity/upper/reverse — the third and fourth
demonstrate that `SET_MODE` sticks until changed again), a deliberately
invalid `SET_MODE` failing with `EINVAL`, a deliberately unknown ioctl
command number failing with `Inappropriate ioctl for device` (`ENOTTY`),
and a final stats read showing accumulated counts. Watch `dmesg -w`
alongside it — every accepted `RESET`/`SET_MODE`/`ECHO` logs the acting
process.

Drive it by hand with `python3` if you want to see the raw values without
`ioctl_test`'s formatting:

```bash
python3 - <<'EOF'
import fcntl, struct

# Layout must match struct ioctl_basics_echo { char buf[64]; } exactly.
IOCTL_BASICS_ECHO = 0xC0406B04  # see "computing the numbers by hand" below

fd = open("/dev/ioctl_basics0", "r+b", buffering=0)
buf = struct.pack("64s", b"from python")
result = fcntl.ioctl(fd, IOCTL_BASICS_ECHO, buf)
print(result.split(b"\x00", 1)[0])
EOF
```

## checkpatch + userspace lint

```bash
make check   # checkpatch --strict on ioctl_basics.c and ioctl_basics.h,
              # then a -Wall -Wextra -Wpedantic -Werror syntax check on ioctl_test.c
```

## Cleanup

```bash
sudo rmmod ioctl_basics
dmesg | tail -3     # final resets=/echoes= counts
make clean            # also removes the ioctl_test binary
```

## Things to try

- Computing the numbers by hand: `_IOWR(IOCTL_BASICS_MAGIC, 4, struct
  ioctl_basics_echo)` expands (via `include/uapi/asm-generic/ioctl.h`) to
  `(dir << 30) | (size << 16) | (type << 8) | nr`, with `dir=3` (both
  read and write), `size=sizeof(struct ioctl_basics_echo)=64`,
  `type='k'=0x6b`, `nr=4`. Compute it yourself and compare against
  `strace -e ioctl ./ioctl_test /dev/ioctl_basics0` while it runs — strace
  decodes the command number back into direction/size/type/nr for you.
- Change `struct ioctl_basics_echo`'s `buf` size in the header without
  rebuilding `ioctl_test` (only the module), then run the *old*
  `ioctl_test` binary against the *new* `.ko`. The encoded size is now a
  mismatch between the two sides — read `_IOC_SIZE()` checks (or their
  absence) in real drivers to see how some validate this and some don't;
  this module's driver does not validate it, trusting the shared header
  instead, which is exactly the failure mode you're now reproducing.
- Add a fifth command, `IOCTL_BASICS_GET_MODE` (`_IOR`, returning just the
  current `__u32 mode`), end to end: header, `case` in the kernel switch,
  and a call from `ioctl_test.c`. This is the shortest path to feeling out
  the whole contract yourself.

## Debugging with GDB

For a full, self-contained, step-by-step session for this module — tmux
pane layout, every command, every output explained — see
[`gdb_walkthrough.md`](gdb_walkthrough.md).

Setup: [`../gdb_debugging.md`](../gdb_debugging.md).

```gdb
(gdb) lx-symbols /home/adiopocere/Desktop/codes/linux-kernel-project
(gdb) break ioctl_basics_ioctl
(gdb) continue
```
```bash
sudo ./ioctl_test /dev/ioctl_basics0
```
```gdb
(gdb) print cmd                # the raw encoded command number
(gdb) print/x cmd               # compare against the _IOC() breakdown in this module's README
(gdb) next                      # step into the matching case
```

The most useful thing to watch here is the `switch (cmd)` dispatch
itself: set the breakpoint, then `continue` through each of `ioctl_test`'s
calls in turn and `print cmd` every time — you'll see the exact encoded
values for `RESET`, `GET_STATS`, `SET_MODE`, `ECHO`, and finally the
deliberately-unknown command that falls through to `default: return
-ENOTTY;`. `finish` after stepping into the `IOCTL_BASICS_ECHO` case
shows the transformed `req.buf` right before it's copied back to
userspace.

`apply_mode_transform()` — the function that actually does the
upper-casing/reversing — has no symbol of its own; verification found
its address inside `ioctl_basics_ioctl` (`ioctl_basics_ioctl+716`), so
it's inlined into the `ECHO` case rather than being a real call. To
watch the transform itself, break on `ioctl_basics_ioctl`, `continue`
until you're in the `IOCTL_BASICS_ECHO` case (`print cmd` to confirm),
then `next` through — the inlined transform's `for` loop is right there
in the same stack frame:

```gdb
(gdb) print req.buf         # the string as it arrived, before transformation
(gdb) next                    # steps straight through apply_mode_transform()'s loop, no separate break needed
(gdb) print req.buf             # transformed - upper-cased or reversed, depending on `mode`
```

**`ioctl_basics_init`/`ioctl_basics_exit`/`ioctl_basics_open`**, and
`struct ioctl_basics_stats`, all verified:

```bash
$ gdb -q -batch -nx -ex "file ioctl_basics.ko" \
    -ex "info line ioctl_basics_init" -ex "info line ioctl_basics_exit" \
    -ex "info line ioctl_basics_open" -ex "ptype struct ioctl_basics_stats" ioctl_basics.ko
Line 159 of "ioctl_basics.c" starts at address 0xb60 <ioctl_basics_init> ...
Line 203 of "ioctl_basics.c" starts at address 0xc90 <ioctl_basics_exit> ...
Line 28 of "ioctl_basics.c" starts at address 0x148 <ioctl_basics_open> ...
type = struct ioctl_basics_stats {
    __u64 reads;
    __u64 echoes;
    __u64 resets;
    __u32 mode;
}
```

`ptype`ing the stats struct once is worth it because it's exactly the
shape `IOCTL_BASICS_GET_STATS` copies to userspace — at any breakpoint
inside the `GET_STATS` case, `print stats` (the module-global instance)
shows precisely what `ioctl_test` is about to receive, before
`copy_to_user()` even runs:

```gdb
(gdb) break ioctl_basics_init
(gdb) continue    # after the usual do_init_module -> insmod -> lx-symbols dance
(gdb) next          # alloc_chrdev_region()
(gdb) next            # cdev_init()/cdev_add()
(gdb) next              # class_create()/device_create()
(gdb) finish
(gdb) break ioctl_basics_exit
(gdb) continue
```
```bash
sudo rmmod ioctl_basics
```
```gdb
(gdb) print stats.resets    # matches this module's own dmesg exit line exactly
(gdb) print stats.echoes
(gdb) finish
```

