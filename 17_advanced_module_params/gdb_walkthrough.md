# GDB walkthrough — 17_advanced_module_params, hands-on, start to finish

`token_bucket.c` uses `module_param_cb()` with a custom `kernel_param_ops`
for every parameter — none of them are passive storage. This walkthrough
breaks on every `.set` callback to watch validation accept and reject
live, on the refill timer to watch state evolve on its own schedule, and
finishes with a real, live `insmod` vs `modprobe` demonstration using the
kernel command line.

Every command below says exactly which pane. One command per step,
always — paste it, wait for the prompt to come back, then the next one.

None of `mode_set`/`capacity_set`/`refill_rate_set`/`refill_fn`/
`consume_write` are `__init`/`__exit` — they're ordinary functions in
plain `.text`, so unlike every earlier module's own init/exit functions,
`break <name>` on any of them works directly right after `lx-symbols`,
no `add-symbol-file` workaround needed. Confirmed statically before ever
touching the VM:

```bash
gdb -q -batch -nx -ex "file token_bucket.ko" \
    -ex "info line mode_set" -ex "info line capacity_set" \
    -ex "info line refill_rate_set" -ex "info line refill_fn" \
    -ex "info line consume_write" token_bucket.ko
```
```
Line 66 of "token_bucket.c" starts at address 0x1f0 <mode_set> and ends at 0x1f8 <mode_set+8>.
Line 115 of "token_bucket.c" starts at address 0x2f8 <capacity_set> and ends at 0x328 <capacity_set+48>.
Line 149 of "token_bucket.c" starts at address 0x3e8 <refill_rate_set> and ends at 0x418 <refill_rate_set+48>.
Line 220 of "token_bucket.c" starts at address 0x4e8 <refill_fn> and ends at 0x4f0 <refill_fn+8>.
Line 240 of "token_bucket.c" starts at address 0x48 <consume_write> and ends at 0x50 <consume_write+8>.
```

---

## Step 0 — start the tmux session

*Regular terminal, not tmux yet.*

```bash
tmux kill-session -t kgdb 2>/dev/null
tmux new-session -d -s kgdb -x 220 -y 50
tmux split-window -h -t kgdb
tmux set -g mouse on
tmux select-pane -t kgdb:0.0 -T vmb
tmux select-pane -t kgdb:0.1 -T gdb
tmux set -t kgdb pane-border-status top
tmux attach -t kgdb
```

Two panes now: **vmb** (left) and **gdb** (right).

## Step 1 — build it, copy onto the scratch disk

*Regular terminal (detach with `Ctrl-b d`, or a separate window).*

```bash
cd /home/adiopocere/Desktop/codes/linux-kernel-project/17_advanced_module_params
make -C /home/adiopocere/Desktop/codes/linux_mainline M=$(pwd) modules
```
```bash
modinfo token_bucket.ko | grep -A6 parm
```
```bash
sudo mount -o loop /home/adiopocere/Desktop/codes/qemu-vmb/labs-disk.img /tmp/vmb-mnt
sudo mkdir -p /tmp/vmb-mnt/17_advanced_module_params
sudo cp token_bucket.ko /tmp/vmb-mnt/17_advanced_module_params/
sudo umount /tmp/vmb-mnt
```

## Step 2 — boot the guest

**Pane: vmb**

```bash
qemu-system-aarch64 -M virt -cpu max -m 1024 -smp 2 \
  -kernel /home/adiopocere/Desktop/codes/linux_mainline/arch/arm64/boot/Image \
  -initrd /home/adiopocere/Desktop/codes/qemu-vmb/initramfs.cpio.gz \
  -drive file=/home/adiopocere/Desktop/codes/qemu-vmb/labs-disk.img,if=virtio,format=raw \
  -append "console=ttyAMA0 rdinit=/init nokaslr" -nographic -s
```

Wait for `=== VM B (QEMU) ready ===` and `~ #`.

## Step 3 — start gdb, connect, load the module

**Pane: gdb**

```bash
cd /home/adiopocere/Desktop/codes/linux_mainline && gdb -q -iex 'set auto-load safe-path /' vmlinux
```
```
target remote :1234
```
```
break do_init_module
```
```
continue
```

**Pane: vmb**

```bash
insmod /mnt/labs/17_advanced_module_params/token_bucket.ko
```

**Pane: gdb**

```
Thread 2 hit Breakpoint 1, do_init_module (mod=mod@entry=0x...) at kernel/module/main.c:3089
```
```
lx-symbols /home/adiopocere/Desktop/codes/linux-kernel-project
```

Set every breakpoint this walkthrough needs up front, then let the
module finish loading:

```
break mode_set
```
```
break capacity_set
```
```
break refill_rate_set
```
```
continue
```

**Pane: vmb**

```
[   ...] token_bucket: 'unnamed-limiter' up - capacity=100 refill_rate=5 mode=monitor
~ #
```

---

## Step 4 — `mode`: a validated enum, watched live

**Pane: vmb**

```bash
echo enforce > /sys/module/token_bucket/parameters/mode
```

**Pane: gdb**

```
Thread 2 hit Breakpoint 2, mode_set (val=0x... "enforce\n", kp=0x...)
    at token_bucket.c:66
66	{
```

**`val` already carries a trailing `\n`** — every shell `echo` write does
this; the kernel's `sysfs_streq()` a moment from now is specifically
designed to ignore it when matching, but naive storage of `val` verbatim
would not be. Step through the comparison chain:

```
next
```
```
print mode
```
```
$1 = MODE_MONITOR
```

Still the old value — none of the `if`/`else if` checks have run yet.

```
next
```
```
next
```
```
next
```

Each `next` falls through one rejected `sysfs_streq()` (`"off"`, then
`"monitor"`) before landing in the `"enforce"` branch. One more `next`
steps past both the enum assignment and the canonical-string setup in
one go (the compiler merged them — nothing to see mid-branch, GDB just
resolves at the next real line boundary):

```
next
```
```
print mode_str
```
```
$2 = "enforce\000\000\000\000\000\000\000\000"
```

`mode_str` holds the **canonical** literal `"enforce"`, not `val`
verbatim — this is deliberate, not an accident of `strscpy()`. Storing
`val` directly here would carry its trailing `\n` straight into
`mode_str`, and `mode_get()`'s own `sysfs_emit(buffer, "%s\n", mode_str)`
would then add a *second* newline on readback. Confirmed for real while
building this module — before this fix, `echo enforce > .../mode` then
`od -c .../mode` read back `"enforce\n\n"`, two newlines. Storing the
canonical string instead of `val` is the actual fix, not a workaround.

```
continue
```

**Pane: vmb**

```bash
cat /sys/module/token_bucket/parameters/mode
```
```
enforce
```

One newline, from `cat` itself — confirmed fixed.

## Step 5 — `mode`: the rejection path

**Pane: vmb**

```bash
echo bogus > /sys/module/token_bucket/parameters/mode
```

**Pane: gdb**

```
Thread 2 hit Breakpoint 2, mode_set (val=0x... "bogus\n", kp=0x...)
    at token_bucket.c:66
```
```
next
```
```
next
```
```
next
```
```
next
```

Four `next`s fall all the way through every `sysfs_streq()` check —
`"off"`, `"monitor"`, `"enforce"` all fail against `"bogus"` — landing in
the `else` branch, `return -EINVAL`. `mode`/`mode_str` are untouched;
confirm from the source directly (`token_bucket.c:78-80`, the `else`
branch returning `-EINVAL` before any store ever happens) rather than by
guessing.

```
continue
```

**Pane: vmb**

```
sh: write error: Invalid argument
```

## Step 6 — `refill_rate`: validated against `capacity`'s *live* value

**Pane: vmb**

```bash
echo 500 > /sys/module/token_bucket/parameters/refill_rate
```

**Pane: gdb**

```
Thread 2 hit Breakpoint 4, refill_rate_set (val=0x... "500\n", kp=0x...)
    at token_bucket.c:149
```
```
next
```
```
next
```
```
next
```
```
next
```
```
next
```

(Five `next`s: the variable declarations, `kstrtoint()` parsing `"500\n"`
into `new_rate`, the `<= 0` guard, and stepping into `spin_lock_irqsave`'s
inlined body — GDB follows inlined code as part of the same frame, one
extra `next` there is normal.)

```
next
```
```
161		reject = new_rate > capacity;
```

That's the line this whole section is about. One more `next` executes
it:

```
next
```
```
print reject
```
```
$3 = true
```
```
print capacity
```
```
$4 = 100
```
```
print refill_rate
```
```
$5 = 5
```

**`refill_rate` is still `5`, the old value** — `reject` came back `true`
because `new_rate(500) > capacity(100)`, and the `if (!reject)
refill_rate = new_rate;` right after never ran. This is the module's
central point made concrete: `refill_rate_set()` reached into
`capacity`'s *current* value to make its decision — two parameters that
are not independent.

```
continue
```

**Pane: vmb**

```
sh: write error: Invalid argument
```

---

## Step 7 — `capacity`: a live side effect, not just storage

**Pane: vmb**

```bash
cat /sys/module/token_bucket/parameters/tokens_available
```
```
100
```

**Pane: gdb**

Add one more breakpoint, right at the clamp check itself, before
triggering the write:

```
break token_bucket.c:127
```

**Pane: vmb**

```bash
echo 30 > /sys/module/token_bucket/parameters/capacity
```

**Pane: gdb**

```
Thread 2 hit Breakpoint 3, capacity_set (val=0x... "30\n", kp=0x...)
    at token_bucket.c:115
```
```
continue
```
```
Thread 2 hit Breakpoint 5, capacity_set (val=<optimized out>, kp=<optimized out>)
    at token_bucket.c:127
127		if (tokens > capacity)
```

```
print tokens
```
```
$6 = 100
```
```
print capacity
```
```
$7 = 30
```

`capacity` is already `30` — the assignment on the line just above (126)
already ran. `tokens` is still `100`. `100 > 30` is true, so:

```
next
```
```
128			tokens = capacity;
```
```
print tokens
```
```
$8 = 100
```

Still `100` — this is the line *about* to run, not the result.

```
next
```
```
print tokens
```
```
$9 = 30
```

**Clamped, live, under lock** — the exact instant the write landed.
Nothing else touched `tokens`; this is `capacity_set()`'s own side
effect, watched at the source line that causes it.

```
continue
```

**Pane: vmb**

```bash
cat /sys/module/token_bucket/parameters/tokens_available
```
```
30
```

---

## Step 8 — the refill timer: state that changes on its own

**Pane: gdb**

```
delete
```
```
y
```
```
break refill_fn
```
```
continue
```

Don't touch `vmb` — `refill_fn` fires on its own, once a second, whether
you're watching or not:

```
Thread 1 hit Breakpoint 6, refill_fn (t=0x... <refill_timer>) at token_bucket.c:220
```
```
print tokens
```
```
$10 = 30
```

Already at `capacity` (`30`) from step 7 — nothing was consumed since.

```
next
```
```
next
```
```
224		tokens = min(tokens + refill_rate, capacity);
```
```
next
```
```
print tokens
```
```
$11 = 30
```

**Still `30`, not `35`.** `min(tokens + refill_rate, capacity)` — with
`tokens` already at the ceiling, the refill is a no-op every tick until
something actually spends tokens. This `min()` is the only thing standing
between "add `refill_rate` every tick forever" and a bucket that
correctly never exceeds its own `capacity`.

```
continue
```

---

## Step 9 — `consume`: spending a token through the misc device

**Pane: gdb**

```
delete
```
```
y
```
```
break consume_write
```
```
continue
```

**Pane: vmb**

```bash
echo x > /dev/token_bucket_consume
```

**Pane: gdb**

```
Thread 1 hit Breakpoint 7, consume_write (f=0x..., buf=0x... "x\n...", len=2, off=0x...)
    at token_bucket.c:240
```
```
print tokens
```

Whatever `tokens` currently is — `> 0` if the timer's kept it topped up,
which it will have (step 8 showed why). Step into the spend:

```
next
```
```
next
```
```
print had_token
```
```
$13 = true
```

`had_token` is true, so `tokens--` and `total_consumed++` run, not the
rejection branch.

```
continue
```

**Pane: vmb**

```bash
cat /sys/module/token_bucket/parameters/tokens_available
```

One less than before this step.

**The rejection path** (`had_token == false && mode == MODE_ENFORCE` →
`-EBUSY`) is real and already confirmed — just via direct functional
testing rather than a breakpoint, since catching the exact
zero-tokens instant through a debugger session (where every pause you
take gives the once-a-second timer more time to refill) is a genuinely
awkward race to win by hand. With `mode` still `enforce` from step 4:

```bash
for i in $(seq 1 60); do echo x > /dev/token_bucket_consume 2>/dev/null; done
echo x > /dev/token_bucket_consume
```
```
sh: write error: Device or resource busy
```

`-EBUSY`, confirmed. Switch `mode` back to `monitor` and the identical
zero-token write succeeds every time — `consume_write()`'s own source
(`token_bucket.c:254`) is the complete explanation for the difference,
already read in Step 6's neighborhood.

## Step 10 — clean up this session

**Pane: gdb**

```
delete
```
```
y
```

**Pane: vmb**

```bash
rmmod token_bucket
```
```
[   ...] token_bucket: 'unnamed-limiter' down - consumed=N rejected=N
```
```bash
poweroff -f
```

**Pane: gdb**

```
quit
```
```
y
```

---

## Part B — `insmod` never reads `/proc/cmdline`; `modprobe` does

No gdb needed for this part — a fresh boot with the parameter set on the
kernel command line, then a plain `insmod`.

### Step 11 — boot with `token_bucket.mode=enforce` on the command line

**Pane: vmb**

```bash
qemu-system-aarch64 -M virt -cpu max -m 1024 -smp 2 \
  -kernel /home/adiopocere/Desktop/codes/linux_mainline/arch/arm64/boot/Image \
  -initrd /home/adiopocere/Desktop/codes/qemu-vmb/initramfs.cpio.gz \
  -drive file=/home/adiopocere/Desktop/codes/qemu-vmb/labs-disk.img,if=virtio,format=raw \
  -append "console=ttyAMA0 rdinit=/init nokaslr token_bucket.mode=enforce" -nographic -s
```

Wait for `~ #`.

```bash
cat /proc/cmdline
```
```
console=ttyAMA0 rdinit=/init nokaslr token_bucket.mode=enforce
```

The parameter really is there, in the exact `<module>.<option>` form
`modprobe`'s own man page (`man modprobe`, "will also use module options
specified on the kernel command line") describes reading.

### Step 12 — `insmod` it anyway, and watch it *not* apply

```bash
insmod /mnt/labs/17_advanced_module_params/token_bucket.ko
```
```bash
cat /sys/module/token_bucket/parameters/mode
```
```
monitor
```

**Not `enforce`.** `mode` came up at its plain compiled-in default,
exactly as if `token_bucket.mode=enforce` were never on the command line
at all. This minimal guest's `insmod` is the classic, direct syscall
wrapper — it hands the kernel exactly the parameters you typed after the
filename and nothing else. The `/proc/cmdline`-scraping step described in
`modprobe`'s man page is `modprobe`'s own userspace logic, layered on top
of `insmod`'s underlying `finit_module(2)` syscall, not something the
kernel or `insmod` itself ever does. This busybox initramfs has no
`/lib/modules` tree for `modprobe` to work with, so there's no way to
demonstrate the *positive* case here — but the negative case just proven
is exactly why that distinction matters: a parameter sitting right there
in `/proc/cmdline`, silently not applied.

### Step 13 — clean up

```bash
rmmod token_bucket
```
```bash
poweroff -f
```

---

## What this proves

- Every parameter here uses `module_param_cb()` because plain
  `module_param()` cannot validate (`mode`), cross-check against a
  sibling parameter (`refill_rate` vs `capacity`), or trigger a live side
  effect (`capacity`'s clamp) — confirmed live, at the exact source lines
  responsible, not asserted from the macro's shape alone (steps 4-7).
- Storing the canonical matched string instead of the raw sysfs input
  (step 4) isn't defensive style — it's the fix for a real, reproduced
  bug (a doubled trailing newline) found while building this module, not
  a hypothetical.
- The refill timer's `min(tokens + refill_rate, capacity)` (step 8) is
  the only thing enforcing `capacity` as a true ceiling rather than a
  number that's merely checked once at write time.
- `insmod` and `modprobe` are not interchangeable ways of spelling the
  same load operation — one reads `/proc/cmdline` for module parameters,
  the other doesn't, confirmed live by putting a parameter on the kernel
  command line and watching a real `insmod` ignore it completely (Part
  B) — grounded directly in `modprobe`'s own documented behavior, not
  inferred from watching `insmod` alone.
