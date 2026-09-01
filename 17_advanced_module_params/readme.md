# 17 — advanced_module_params

Module 04 covers one example of each basic `module_param*()` flavor —
useful for learning the macros, but every one of those parameters is
just passive storage: write a value, it sits there. This module covers
what real drivers do once parameters need to be more than that: a
token-bucket rate limiter, the same mechanism behind traffic shaping
(`tc`'s `sch_tbf`) and every "don't let this fire more than N times a
second" guard in the kernel — built specifically so its parameters have
to validate input, reject inconsistent combinations, and trigger real
state changes on write, not just store a number.

Every parameter here uses `module_param_cb()` with a custom
`kernel_param_ops`, not the plain `module_param()` macro, because every
one needs something `module_param()` alone can't do. Real-world
reference points, not invented: `drivers/hid/hid-cougar.c`'s
`g6_is_space` parameter (validate via a stock helper, then trigger real
reconfiguration) and `drivers/char/ipmi/ipmi_watchdog.c`'s `action`
parameter (a fixed set of valid strings, `-EINVAL` on anything else) are
both real, shipping kernel code using this exact pattern — both present
in `../../linux_mainline`, grep for `module_param_cb` yourself.

## What this demonstrates

- **`mode`: a validated enum, not a raw type.** `off` / `monitor` /
  `enforce` are the only accepted values (`sysfs_streq()` against each,
  matching `ipmi_watchdog.c`'s `action_op_set_val()` pattern exactly) —
  anything else is rejected with `-EINVAL` before it ever reaches the
  variable. `module_param(mode, charp, ...)` alone cannot do this; it
  would accept any string at all.
- **`refill_rate` validates against `capacity`'s *current* value, not in
  isolation.** This is the module's central point: module parameters are
  not independent knobs. Writing a `refill_rate` greater than the
  bucket's own `capacity` is rejected, live, by reading the sibling
  parameter's value inside the `.set` callback.
- **`capacity` has a real side effect.** Shrinking it below the current
  token count doesn't just change a ceiling for next time — it clamps
  the live count immediately, under the same lock the refill timer and
  the consume path use. A parameter write here does real, immediate,
  concurrency-safe work, not just storage.
- **`tokens_available` is genuinely read-only** (`0444`, no `.set` at
  all) — not "settable but permission-denied," a parameter that
  structurally cannot be written because it isn't independent state to
  begin with. Its `.get` computes the value live, under lock, on every
  read.
- **`label` uses `module_param_string()`, not `charp`** (module 04's
  flavor) — a fixed `LABEL_LEN`-byte buffer the kernel owns directly,
  not a pointer to a string `insmod`/`modprobe` allocated for you.
  Compare `readelf -x .data token_bucket.ko` against module 04's
  `greeting` default: this one's bytes sit in `.data` itself. Try writing
  a label longer than the buffer (see below) — the default `.set` this
  macro installs for you enforces the limit itself, with a real kernel
  log line naming the exact overflow.
- **`insmod` does not scrape `/proc/cmdline` for module parameters —
  only `modprobe` does.** Per the kernel's own documentation
  (kernel.org's `admin-guide/kernel-parameters.html`): "When [modprobe]
  loads a module, it looks through the kernel command line... and
  collects any module parameters." Confirmed live below: booting with
  `token_bucket.mode=enforce` on the QEMU `-append` line and then
  plain-`insmod`-ing this module still comes up in the default `monitor`
  mode — the parameter is genuinely there in `/proc/cmdline`, `insmod`
  simply never looks.
- **Locking discipline around parameter-triggered state.** Every
  callback that touches `tokens` (`capacity_set`, the refill timer, the
  consume path) takes `bucket_lock` first — module 11 covers what
  happens to shared state exactly like this without it; this module is
  the same lesson from the parameter-callback side specifically.

## Files

| File | Purpose |
|---|---|
| `token_bucket.c` | The module: token-bucket rate limiter, five parameters (`mode`, `capacity`, `refill_rate`, `tokens_available`, `label`), a refill timer, a `/dev/token_bucket_consume` misc device. |
| `Makefile` | Build, `clean`, `check`/`checkpatch`. |

## Build

```bash
cd 17_advanced_module_params
make
```

## Load and test

```bash
sudo insmod ./token_bucket.ko
dmesg | tail -1        # capacity=100 refill_rate=5 mode=monitor
```

Permissions confirm the read-only parameter directly:

```bash
ls -la /sys/module/token_bucket/parameters/
# tokens_available is r--r--r--, everything else is rw-r--r--
```

Invalid `mode` is rejected before it's ever stored:

```bash
echo bogus > /sys/module/token_bucket/parameters/mode    # write error: Invalid argument
echo enforce | sudo tee /sys/module/token_bucket/parameters/mode
```

`refill_rate` validated against `capacity`'s live value:

```bash
echo 500 | sudo tee /sys/module/token_bucket/parameters/refill_rate    # write error: Invalid argument (500 > capacity)
echo 10 | sudo tee /sys/module/token_bucket/parameters/refill_rate     # fine (10 <= 100)
```

`capacity`'s live clamp — watch `tokens_available` drop the instant you
shrink it, no reload needed:

```bash
cat /sys/module/token_bucket/parameters/tokens_available    # 100
echo 30 | sudo tee /sys/module/token_bucket/parameters/capacity
cat /sys/module/token_bucket/parameters/tokens_available    # 30, clamped live
```

Spend tokens through the misc device; `enforce` mode rejects the write
once the bucket is empty, `monitor`/`off` never do:

```bash
echo x | sudo tee /dev/token_bucket_consume
cat /sys/module/token_bucket/parameters/tokens_available     # one less
```

`label`'s fixed buffer, and what happens past its limit:

```bash
echo my-limiter | sudo tee /sys/module/token_bucket/parameters/label
cat /sys/module/token_bucket/parameters/label
echo this-is-a-genuinely-way-too-long-label-for-32-bytes | sudo tee /sys/module/token_bucket/parameters/label
# write error: No space left on device
dmesg | tail -1    # "label: string doesn't fit in 31 chars."
```

`insmod` vs `modprobe` and `/proc/cmdline` — needs a fresh boot with the
parameter on the kernel command line to actually test; see
[`gdb_walkthrough.md`](gdb_walkthrough.md)'s cmdline-vs-insmod section
for the full, live QEMU sequence. On this machine directly, `modprobe`'s
own man page
states the behavior `insmod` lacks, in its own words:

```bash
man modprobe | grep -B1 -A2 "will also use module options"
```
```
modprobe will also use module options specified on the ker-
nel command line in the form of <module>.<option> and blacklists in
form of modprobe.blacklist=<module>.
```

## checkpatch

```bash
make check
```

## Cleanup

```bash
sudo rmmod token_bucket
dmesg | tail -1    # consumed=N rejected=N
make clean
```

## Things to try

- Write `capacity` down to `1`, then try `refill_rate` values above and
  below it — the rejection boundary is exactly `refill_rate <=
  capacity`, confirmed by reading `capacity_set`'s comparison directly
  rather than guessing from behavior.
- Compare `hid-cougar.c`'s `cougar_param_set_g6_is_space()` and this
  module's `capacity_set()` side by side in `../../linux_mainline` — both
  follow the identical shape: parse/validate, then trigger real work,
  all inside the `.set` callback.
- Time the refill: with `mode=monitor`, drain the bucket via a tight
  loop against `/dev/token_bucket_consume`, then `watch -n1 cat
  /sys/module/token_bucket/parameters/tokens_available` from a second
  shell and watch it climb back by `refill_rate` roughly once a second.

## Debugging with GDB

A fully self-contained, hands-on walkthrough for this module — tmux
session creation, build, boot, every gdb command, every expected output,
and cleanup, start to finish, no other file needed:
[`gdb_walkthrough.md`](gdb_walkthrough.md). Breaks on every `.set`
callback to watch validation accept and reject live, on the refill timer
to watch the token count evolve under lock on its own schedule, and
includes a real, live-verified reboot with `token_bucket.mode=enforce`
on the kernel command line to prove `insmod` never picks it up.
