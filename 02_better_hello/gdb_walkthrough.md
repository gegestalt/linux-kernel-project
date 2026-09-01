# GDB walkthrough — 02_better_hello, hands-on, start to finish

`better_hello.c` does what `hello.c` (module 01) did, but through the
real mechanism every later module in this repo uses: `module_init(my_init)`
/ `module_exit(my_exit)` macros instead of the magic `init_module`/
`cleanup_module` symbol names, plus `__init`/`__exit` section
annotations. Same two `printk()`s, different debugging mechanics — worth
seeing once, deliberately, here.

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

*Regular terminal (detach with `Ctrl-b d`, or a separate window).*

```bash
cd 02_better_hello
make -C /home/adiopocere/Desktop/codes/linux_mainline M=$(pwd) modules
```

## Step 2 — confirm the alias, statically, before touching a VM

```bash
nm better_hello.ko | grep -i init_module -e my_init
```
```
0000000000000008 T init_module
0000000000000008 t my_init
```

**Both names, same address.** `module_init(my_init)` (`include/linux/module.h`)
expands to `int init_module(void) __copy(initfn) __attribute__((alias(#initfn)));`
— the compiler emits a *second*, global symbol (`init_module`) that's a
hard alias for whatever function you passed the macro. `my_init` stays a
`static` local symbol at the exact same address. `hello.c` (module 01)
only ever had the one name, because it defined `init_module` directly —
no macro, no alias. `kernel/module/main.c` calls `mod->init` either way:

```bash
grep -n "do_one_initcall(mod->init)" /home/adiopocere/Desktop/codes/linux_mainline/kernel/module/main.c
```
```
kernel/module/main.c:3117:            ret = do_one_initcall(mod->init);
```

Same call path as module 01. What's different here is purely a *naming*
fact: `break my_init` resolves to a real, independent symbol only in this
module, because this module's alias leaves the original name in the
symbol table.

## Step 3 — verify the source lines this module will stop on

```bash
gdb -q -batch -nx -ex "file better_hello.ko" -ex "info line my_init" -ex "info line my_exit" better_hello.ko
```
```
Line 9 of "better_hello.c" starts at address 0x38 <my_init> and ends at 0x40 <my_init+8>.
Line 21 of "better_hello.c" starts at address 0x78 <my_exit> and ends at 0x80 <my_exit+8>.
```

## Step 4 — check vermagic, copy onto the scratch disk

```bash
modinfo better_hello.ko | grep vermagic
```
```
vermagic: 7.2.0-kgdb-debug+ SMP preempt mod_unload modversions aarch64
```
```bash
sudo mount -o loop /home/adiopocere/Desktop/codes/qemu-vmb/labs-disk.img /tmp/vmb-mnt
sudo mkdir -p /tmp/vmb-mnt/02_better_hello
sudo cp better_hello.ko /tmp/vmb-mnt/02_better_hello/
sudo umount /tmp/vmb-mnt
```

## Step 5 — boot the guest

**Pane: vmb**

```bash
qemu-system-aarch64 -M virt -cpu max -m 1024 -smp 2 \
  -kernel /home/adiopocere/Desktop/codes/linux_mainline/arch/arm64/boot/Image \
  -initrd /home/adiopocere/Desktop/codes/qemu-vmb/initramfs.cpio.gz \
  -drive file=/home/adiopocere/Desktop/codes/qemu-vmb/labs-disk.img,if=virtio,format=raw \
  -append "console=ttyAMA0 rdinit=/init nokaslr" -nographic -s
```

Wait for `=== VM B (QEMU) ready ===` and `~ #`.

## Step 6 — start gdb, connect

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

## Step 7 — break on the load entry point

**Pane: gdb**

```
break do_init_module
```
```
continue
```

Prints `Continuing.` — switch panes.

## Step 8 — trigger the load

**Pane: vmb**

```bash
insmod /mnt/labs/02_better_hello/better_hello.ko
```

## Step 9 — load symbols, break on the real function name

**Pane: gdb**

```
Thread 2 hit Breakpoint 1, do_init_module (mod=mod@entry=0x...) at kernel/module/main.c:3089
```
```
print mod->name
```
```
$1 = "better_hello", '\000' <repeats ...>
```
```
lx-symbols /home/adiopocere/Desktop/codes/linux-kernel-project
```
```
loading @0x...: .../02_better_hello/better_hello.ko
```

Now break on `my_init` directly — the local alias's own name, not
`init_module`:

```
break my_init
```
```
continue
```
```
Thread 2 hit Breakpoint 2, my_init () at better_hello.c:9
9           printk(KERN_INFO "Hello luv .\n");
```

## Step 10 — confirm the frame shape matches module 01

```
bt
```
```
#0  my_init () at better_hello.c:9
#1  0x... in do_one_initcall (fn=0x... <init_module>) at init/main.c:...
#2  0x... in do_init_module (mod=0x...) at kernel/module/main.c:...
...
```

Same shape as module 01 — `do_one_initcall()` calls `mod->init` for
*every* module regardless of which macro it used. Worth noticing: frame
`#1`'s `fn=` prints as `<init_module>` even though you broke on
`my_init` and frame `#0` shows `my_init` — both names point at the
identical address (step 2), GDB just picked one label. `print/x $pc` and
`info symbol $pc` in frame 0 would show both names resolving to the same
value if you want to check.

```
finish
```
```
Run till exit from #0  my_init () at better_hello.c:9
Value returned is $2 = 0
```

## Step 11 — the exit path: where it actually differs from module 01

`my_exit` is marked `__exit`, placed in its own `.exit.text` section,
separate from `.text`. `lx-symbols` only relocates a fixed, hardcoded
list of sections — `.exit.text` isn't one of them. So this looks fine at
first:

```
break my_exit
```
```
Breakpoint 3 at 0x58: file better_hello.c, line 21.
```

That `0x58` is a **raw, unrelocated file offset**, not a real kernel
address — silently wrong, no error. Confirm the fix works by breaking on
the generic syscall hook every `rmmod` goes through instead:

```
break __do_sys_delete_module
```
```
continue
```

**Pane: vmb**

```bash
rmmod better_hello
```

**Pane: gdb**

```
Thread 2 hit Breakpoint 4, __do_sys_delete_module (flags=..., name_user=...) at kernel/module/main.c:808
```
```
advance kernel/module/main.c:863
```
```
__do_sys_delete_module (...) at kernel/module/main.c:863
863         mod->exit();
```
```
print mod->exit
```
```
$3 = (void (*)(void)) 0xffff80007c320028
```

(That exact address is from one real run — yours will differ, module
memory is placed fresh each boot even with `nokaslr`. Use whatever
`print mod->exit` gives you next.)

```
add-symbol-file /home/adiopocere/Desktop/codes/linux-kernel-project/02_better_hello/better_hello.ko -s .exit.text 0xffff80007c320028
```
```
y
```
```
break my_exit
```
```
Breakpoint 5 at 0x58: my_exit. (2 locations)
```

Two locations now — `5.1` is still the old broken raw-offset one, `5.2`
is the newly-relocated real one:

```
disable 5.1
```
```
continue
```
```
Thread 2 hit Breakpoint 5.2, 0x... in cleanup_module ()
```

Reports as `cleanup_module`, not `my_exit` — `module_exit()` aliases to
the legacy name too, same mirror-image of step 2.

```
bt
```

Full `delete_module(2)` syscall chain down to the arm64 syscall entry
trampoline.

## Step 12 — clean up

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

- `module_init()`/`module_exit()` are alias-generating macros, not a
  different call mechanism — `nm`'s two symbols at one address (step 2)
  is direct proof, and the identical `do_one_initcall()` → `mod->init`
  call path (step 10) shows the runtime behavior really is the same as
  module 01's.
- `break my_init` works by the name in your own source, instead of
  every module in the world sharing the one generic `init_module` name
  module 01 used — the one real payoff of the macro.
- `__exit`-annotated functions live in `.exit.text`, which `lx-symbols`
  never relocates — `break my_exit` silently resolves to a bogus raw
  file offset unless you manually `add-symbol-file` it against
  `mod->exit`'s real, live address first (step 11). Every module from
  here on hits this the same way.
