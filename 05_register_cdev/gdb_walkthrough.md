# GDB walkthrough — 05_register_cdev, hands-on, start to finish

`register_cdev.c` is the legacy, one-call char device registration path
— `register_chrdev()` allocates a major dynamically and wires up
`file_operations` in a single step, with no `struct cdev`, no device
class, and no automatic `/dev` node — you `mknod` it yourself once
you've read the major out of `dmesg`. The debugging focus here is the
**open/read/release triangle**: three separate callbacks, invoked by
three separate syscalls, that all need to agree on `struct inode`/
`struct file` identity — plus an `atomic_t` open-count, the simplest
possible shared-state example before module 11 covers real races.

Every command below says exactly which pane. One command per step,
always — paste it, wait for the prompt to come back, then the next one.

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

## Step 1 — build it

*Regular terminal.*

```bash
cd /home/adiopocere/Desktop/codes/linux-kernel-project/05_register_cdev
make -C /home/adiopocere/Desktop/codes/linux_mainline M=$(pwd) modules
```

## Step 2 — verify the breakpoint targets, statically

```bash
gdb -q -batch -nx -ex "file register_cdev.ko" \
    -ex "info line register_cdev_open" -ex "info line register_cdev_release" \
    -ex "info line register_cdev_init" -ex "ptype open_count" register_cdev.ko
```
```
Line 26 of "register_cdev.c" starts at address 0x188 <register_cdev_open> ...
Line 97 of "register_cdev.c" starts at address 0xc8 <register_cdev_release> ...
Line 127 of "register_cdev.c" starts at address 0x8b0 <register_cdev_init> ...
type = struct {
    int counter;
}
```

`open_count` really is just a one-member struct wrapping a plain `int` —
the "atomic" part is entirely in *how* `atomic_inc_return()`/
`atomic_dec_return()` touch that `int` (a single hardware-guaranteed
instruction), not in the type looking any different from a normal
counter.

## Step 3 — check vermagic, copy onto the scratch disk

```bash
modinfo register_cdev.ko | grep vermagic
```
```
vermagic: 7.2.0-kgdb-debug+ SMP preempt mod_unload modversions aarch64
```
```bash
sudo mount -o loop /home/adiopocere/Desktop/codes/qemu-vmb/labs-disk.img /tmp/vmb-mnt
sudo mkdir -p /tmp/vmb-mnt/05_register_cdev
sudo cp register_cdev.ko /tmp/vmb-mnt/05_register_cdev/
sudo umount /tmp/vmb-mnt
```

## Step 4 — boot the guest

**Pane: vmb**

```bash
qemu-system-aarch64 -M virt -cpu max -m 1024 -smp 2 \
  -kernel /home/adiopocere/Desktop/codes/linux_mainline/arch/arm64/boot/Image \
  -initrd /home/adiopocere/Desktop/codes/qemu-vmb/initramfs.cpio.gz \
  -drive file=/home/adiopocere/Desktop/codes/qemu-vmb/labs-disk.img,if=virtio,format=raw \
  -append "console=ttyAMA0 rdinit=/init nokaslr" -nographic -s
```

Wait for `=== VM B (QEMU) ready ===` and `~ #`.

## Step 5 — start gdb, connect

**Pane: gdb**

```bash
cd /home/adiopocere/Desktop/codes/linux_mainline && gdb -q -iex 'set auto-load safe-path /' vmlinux
```
```
target remote :1234
```
```
lx-version
```

## Step 6 — break on the load entry point

**Pane: gdb**

```
break do_init_module
```
```
continue
```

Switch panes.

## Step 7 — trigger the load

**Pane: vmb**

```bash
insmod /mnt/labs/05_register_cdev/register_cdev.ko
```

## Step 8 — load symbols, break inside init, watch the dynamic major get allocated

**Pane: gdb**

```
lx-symbols /home/adiopocere/Desktop/codes/linux-kernel-project
```

**`break register_cdev_init` does not work here — confirmed live (this
exact mechanism, on two other modules), don't trust it.**
`register_cdev_init` is `__init`, placed in `.init.text`. `lx-symbols`
relocates only a fixed, hardcoded list of sections
(`scripts/gdb/linux/symbols.py`'s `_section_arguments()`: `.data`,
`.data..read_mostly`, `.rodata`, `.bss`, `.text.hot`, `.text.unlikely`)
— `.init.text` isn't one of them, the same root cause as step 12's
`.exit.text` problem below, just hitting the *load* path instead.
`break register_cdev_init` right now would be accepted with no error
and silently resolve to a tiny, bogus, unrelocated file offset — it
would never actually fire; `insmod` would run straight through with
nothing caught.

The fix mirrors step 12's exit-path fix exactly, using `mod->init`
instead of `mod->exit` — you're still stopped inside `do_init_module`
right now (step 6), before `do_one_initcall(mod->init)` has run, so
`mod->init` is already the module's real, live init-function address:

```
print mod->init
```
```
$1 = (int (*)(void)) 0x...
```

(That address is from one real run — module memory is placed fresh each
boot even with `nokaslr`. Use whatever `print mod->init` gives you
next.)

```
add-symbol-file /home/adiopocere/Desktop/codes/linux-kernel-project/05_register_cdev/register_cdev.ko -s .init.text 0x...
```
```
y
```
```
break register_cdev_init
```
```
Breakpoint N at 0x...: register_cdev_init. (2 locations)
```

Two locations — `N.1` is the old broken raw-offset one, `N.2` is the
newly-relocated real one:

```
disable N.1
```
```
continue
```
```
Thread 2 hit Breakpoint N.2, 0x... in init_module ()
```

Reports as `init_module`, not `register_cdev_init` — the same alias
mechanics documented in module 02's walkthrough (`module_init()`
generates a hard alias to the legacy name). `add-symbol-file` here only
supplies a symbol address, not full compiler-generated debug info, so
this exact landing point has no source line attached — that clears up
immediately once you step forward:

```
next
```
```
print major
```
```
$2 = 240
```

**What this shows:** the `0` passed as `register_chrdev()`'s first
argument is a request for a *dynamically* assigned major — `major` is
whatever the kernel actually handed back, which can differ run to run
depending on what else has claimed a major number. That's exactly why
the driver `pr_info()`s it: there's no other way for userspace to know
it. You now know it before `dmesg` even shows the line, straight from
the variable.

```
finish
```

**Pane: vmb**

```bash
dmesg | tail -2
```
```bash
mknod /dev/register_cdev0 c $(dmesg | grep -o 'major=[0-9]*' | tail -1 | cut -d= -f2) 0
```

## Step 9 — `open()`: `struct inode`/`struct file` as GDB sees them

**Pane: gdb**

```
break register_cdev_open
```
```
continue
```

**Pane: vmb**

```bash
cat /dev/register_cdev0 &
```

**Pane: gdb**

```
Thread 2 hit Breakpoint N, register_cdev_open (inode=0x..., file=0x...) at register_cdev.c:26
```
```
print inode->i_rdev
```
```
$3 = 61440
```

(`imajor()`/`iminor()` are ordinary inline functions — don't try `print
imajor(inode)` as an actual live function call against a KGDB target,
it isn't reliable there. Reading `i_rdev` directly, the same field those
inlines read, works fine.)

```
print current->comm
```
```
$4 = "cat\000\000\000\000\000\000\000\000\000\000\000\000"
```
```
print current->pid
```

**What this shows:** `current` really is `cat`'s own `task_struct` —
`open()` runs in the calling process's own context, synchronously, which
is why `current` is meaningful at all inside a syscall-driven callback.
(Contrast with module 03's workqueue callback, where `current` was a
`kworker` instead of anything the user typed.)

```
next
```
```
print count
```

## Step 10 — two opens at once: the atomic counter earns its keep

Background a second reader before releasing the first, so `open_count`
genuinely reaches 2 rather than bouncing straight back to 0/1:

```
continue
```

**Pane: vmb**

```bash
cat /dev/register_cdev0 &
```

**Pane: gdb**

```
Thread 2 hit Breakpoint N, register_cdev_open (...) at register_cdev.c:26
```
```
next
```
```
print count
```
```
$5 = 2
```

`atomic_inc_return()` is what makes this number trustworthy even if two
opens happened at genuinely the same instant on different CPUs — worth
explicitly contrasting with module 11, which is entirely about what goes
wrong when a shared counter *isn't* touched atomically.

## Step 11 — `read()`: the kernel-stack buffer, and EOF

```
break register_cdev_read
```
```
continue
```

(Both backgrounded `cat`s from step 10 will already issue their first
`read()` — `continue` past those hits if they land first.)

```
Thread 2 hit Breakpoint N, register_cdev_read (...) at register_cdev.c:46
```
```
next
```
```
print message
```
```
$6 = "register_cdev kernel device\nmajor=240\nminor=0\ncontext=cat[123]\n"
```
```
print &message
```

**What this shows:** `message` is a plain local array — `print &message`
shows an address on the *current kernel stack*, not heap memory; it
exists only for the duration of this one `read()` call and differs on
every call, including from the exact same process. Compare that with
`open_count` — a real global — which keeps the same address for the
module's whole lifetime.

```
next
```
```
print bytes_to_copy
```
```
continue
```

The `cat` this is serving will call `read()` a second time after
consuming the message — catch that second hit and watch `*offset >=
message_length` evaluate true, taking the `return 0;` path immediately:
that `0` is precisely what tells `cat` "end of file, stop reading," the
same convention every regular file relies on.

## Step 12 — clean up: the `__exit` relocation gotcha, again

`register_cdev_exit` is `__exit`, placed in `.exit.text`, which
`lx-symbols` never relocates — same underlying cause documented in
[02_better_hello's walkthrough](../02_better_hello/gdb_walkthrough.md#step-11--the-exit-path-where-it-actually-differs-from-module-01).

```
delete
```
```
y
```
```
break __do_sys_delete_module
```
```
continue
```

**Pane: vmb**

```bash
rmmod register_cdev
```

**Pane: gdb**

```
Thread 1 hit Breakpoint N, __do_sys_delete_module (...) at kernel/module/main.c:808
```
```
advance kernel/module/main.c:863
```
```
863         mod->exit();
```
```
print mod->exit
```
```
$7 = (void (*)(void)) 0xffff80007c32b4d0
```

(That address is from one real run — module memory placement is random
per boot even with `nokaslr`. Use whatever `print mod->exit` gives you
next.)

```
add-symbol-file /home/adiopocere/Desktop/codes/linux-kernel-project/05_register_cdev/register_cdev.ko -s .exit.text 0xffff80007c32b4d0
```
```
y
```
```
break register_cdev_exit
```
```
Breakpoint N at 0x1b0: register_cdev_exit. (2 locations)
```

Two locations — `N.1` is the old broken raw-offset one, `N.2` is the
newly-relocated real one:

```
disable N.1
```
```
continue
```

**Pane: vmb**

```bash
rmmod register_cdev
```

**Pane: gdb**

```
Thread 1 hit Breakpoint N.2, 0xffff80007c32b4d4 in cleanup_module ()
```
```
next
```
```
print open_count
```
```
$8 = {counter = 0}
```

Both backgrounded `cat`s should have already exited (EOF closes their
fd, dropping `open_count` back toward 0) — if it isn't 0 here, one of
them is still holding the device open somewhere, which `rmmod` would
otherwise still permit (this driver has no refcounting tied to
`fops.owner` beyond the module reference itself — worth comparing to
module 12's exit path, which explicitly reasons about exactly this).

```
finish
```

## Step 13 — clean up

**Pane: gdb**

```
delete
```
```
y
```

**Pane: vmb**

```bash
poweroff -f
```

**Pane: gdb**

```
quit
```

---

## What this proves

- The same `struct inode *`/`struct file *` pair travels through
  `open()`, every `read()`, and `release()` for one open file
  description — `current->pid`/`current->comm` at each stop shows
  directly which process is on the other end of that syscall (steps
  9–11).
- `message[]`'s stack address changing between calls (step 11) is
  direct, visible proof that a local array is genuinely reallocated (in
  the "new stack frame" sense) on every invocation, unlike `open_count`
  — a real global that keeps the same address for the module's whole
  lifetime.
- `register_chrdev(0, ...)`'s dynamic major only exists at runtime — the
  driver has to `pr_info()` it because there's no other way for
  userspace to learn it, confirmed by reading it straight out of the
  live `major` variable before `dmesg` even shows the line (step 8).
