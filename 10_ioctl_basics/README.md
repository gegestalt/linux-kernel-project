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
  this lab's driver does not validate it, trusting the shared header
  instead, which is exactly the failure mode you're now reproducing.
- Add a fifth command, `IOCTL_BASICS_GET_MODE` (`_IOR`, returning just the
  current `__u32 mode`), end to end: header, `case` in the kernel switch,
  and a call from `ioctl_test.c`. This is the shortest path to feeling out
  the whole contract yourself.

## Debugging with GDB

Setup: [`../GDB_DEBUGGING.md`](../GDB_DEBUGGING.md).

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
(gdb) print/x cmd               # compare against the _IOC() breakdown in this lab's README
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

## Tracing this live

Setup and general method: [`../FTRACE_TRACING.md`](../FTRACE_TRACING.md).
`ioctl_basics_ioctl(struct file *file, unsigned int cmd, unsigned long arg)`
puts the raw encoded command number at `arg1`:

```bash
sudo bpftrace -e 'kprobe:ioctl_basics:ioctl_basics_ioctl { printf("ioctl cmd=0x%x by %s[%d]\n", arg1, comm, pid); }' &
sleep 1.5
sudo ./ioctl_test /dev/ioctl_basics0
```

Real captured output — the entire `ioctl_test` run, all 10 calls, live:

```
Attached 1 probe
ioctl cmd=0x6b01 by ioctl_test[161624]        # RESET (_IO)
ioctl cmd=0x80206b02 by ioctl_test[161624]    # GET_STATS (_IOR)
ioctl cmd=0xc0406b04 by ioctl_test[161624]    # ECHO identity (_IOWR)
ioctl cmd=0x40046b03 by ioctl_test[161624]    # SET_MODE upper (_IOW)
ioctl cmd=0xc0406b04 by ioctl_test[161624]    # ECHO upper
ioctl cmd=0x40046b03 by ioctl_test[161624]    # SET_MODE reverse
ioctl cmd=0xc0406b04 by ioctl_test[161624]    # ECHO reverse
ioctl cmd=0x40046b03 by ioctl_test[161624]    # SET_MODE invalid (still reaches the kernel - EINVAL happens inside)
ioctl cmd=0x6b63 by ioctl_test[161624]        # the deliberately-unknown command
ioctl cmd=0x80206b02 by ioctl_test[161624]    # GET_STATS (final)
```

Every command number here matches this lab's own `_IOC()` breakdown
exactly (`0x6b` = `'k'`, the low byte is `nr`) — computed live by the
kernel, not asserted in the README. Compare against
`strace -e ioctl ./ioctl_test /dev/ioctl_basics0`, which decodes the
same numbers back into direction/size/type/nr independently.

