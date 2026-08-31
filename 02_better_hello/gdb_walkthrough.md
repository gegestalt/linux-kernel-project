# GDB walkthrough — 02_better_hello

`better_hello.c` does exactly what `hello.c` did, but through the
modern, idiomatic mechanism: `module_init(my_init)` /
`module_exit(my_exit)` macros instead of the magic `init_module`/
`cleanup_module` symbol names, and `__init`/`__exit` section
annotations on the functions themselves. Functionally identical output
— two `printk()`s — but the debugging *mechanics* differ in a way
that's worth seeing deliberately, once, before every later module takes
this pattern for granted.

## What's actually different here, debugger-wise

It's tempting to assume `module_init(my_init)` just renames `my_init`
to `init_module`, in which case this module would debug identically to
01 and there'd be nothing new to see. Check that assumption against the
real, built object rather than the macro's doc-comment:

```
$ nm better_hello.ko | grep -i init_module -e my_init
0000000000000008 T init_module
0000000000000008 t my_init
```

**Both names exist, at the same address.** `include/linux/module.h`'s
real definition of `module_init()` for a loadable module (as opposed to
something built into the kernel image) is:

```c
#define module_init(initfn)                                    \
        static inline initcall_t __maybe_unused __inittest(void) \
        { return initfn; }                                     \
        int init_module(void) __copy(initfn)                   \
                __attribute__((alias(#initfn)));                \
        ___ADDRESSABLE(init_module, __initdata);
```

`alias(#initfn)` makes the compiler emit a *second*, global ELF symbol
(`init_module`) that's a hard alias for whatever function you actually
passed to the macro — `my_init` stays as a `static` (local, lowercase
`t`) symbol at the very same address. So `better_hello.ko` genuinely
has two names for one function; `hello.c` only ever had one, because it
defined `init_module` directly with no macro and no alias involved.
`do_init_module()` in `kernel/module/main.c` calls `mod->init`, which
the module loader populates from the `init_module` symbol either way —
confirmed directly in this tree:

```
kernel/module/main.c:3116:    if (mod->init != NULL)
kernel/module/main.c:3117:            ret = do_one_initcall(mod->init);
```

So the *call path* — `do_init_module()` → `do_one_initcall()` → your
function — is identical between modules 01 and 02; don't expect a
different-shaped backtrace here than you saw there. What actually
differs is purely a *naming* fact you can now demonstrate directly:
`break my_init` resolves to a real, independent symbol only in this
module, because only this module's macro-generated alias leaves the
original semantic name in the symbol table for GDB to find.

`__init`/`__exit` on `my_init`/`my_exit` are a separate detail worth
recognizing on sight from here on: they place the functions in the
`.init.text`/`.exit.text` sections, which the kernel reclaims shortly
after a *built-in* driver's init runs (a `.ko` you can `rmmod`, like
every module in this repo, is unloaded as a whole unit instead, so the
annotation has no visible effect on any module here — but every real driver
you'll read in the mainline tree uses it, so it's worth knowing what it
means when you see it).

## Environment

```bash
cd 02_better_hello
make -C /home/adiopocere/Desktop/codes/linux_mainline M=$(pwd) modules
modinfo better_hello.ko | grep vermagic
sudo mount -o loop /home/adiopocere/Desktop/codes/qemu-vmb/labs-disk.img /tmp/vmb-mnt
sudo mkdir -p /tmp/vmb-mnt/02_better_hello
sudo cp better_hello.ko /tmp/vmb-mnt/02_better_hello/
sudo umount /tmp/vmb-mnt
```

## tmux layout

Same two sessions as every module — `vmb` (guest) and `gdbsess` (GDB) —
see [`../gdb_debugging.md`](../gdb_debugging.md). `target remote
:1234` then `lx-version` first, always.

## The walkthrough

```gdb
(gdb) target remote :1234
(gdb) lx-version
(gdb) break do_init_module
(gdb) continue
```

```bash
# vmb:
insmod /mnt/labs/02_better_hello/better_hello.ko
```

```
Thread 2 hit Breakpoint 1, do_init_module (mod=mod@entry=0x...) at kernel/module/main.c:3089
(gdb) print mod->name
$1 = "better_hello", '\000' <repeats ...>
(gdb) lx-symbols /home/adiopocere/Desktop/codes/linux-kernel-project
loading @0x...: .../02_better_hello/better_hello.ko
```

Now break directly on `my_init` — the local alias's own name, distinct
from `init_module` purely at the symbol-table level even though it's
the same address:

```gdb
(gdb) break my_init
```

Verified statically against the built module before ever touching the
VM:

```
$ gdb -q -batch -nx -ex "file better_hello.ko" -ex "info line my_init" -ex "info line my_exit" better_hello.ko
Line 9 of "better_hello.c" starts at address 0x38 <my_init> and ends at 0x40 <my_init+8>.
Line 21 of "better_hello.c" starts at address 0x78 <my_exit> and ends at 0x80 <my_exit+8>.
```

```gdb
(gdb) continue
Thread 2 hit Breakpoint 2, my_init () at better_hello.c:9
9           printk(KERN_INFO "Hello luv .\n");
(gdb) bt
#0  my_init () at better_hello.c:9
#1  0x... in do_one_initcall (fn=0x... <init_module>) at init/main.c:...
#2  0x... in do_init_module (mod=0x...) at kernel/module/main.c:...
...
```

This is the same shape you'd see in module 01 — `do_one_initcall()` calls
`mod->init` for *every* module regardless of which macro it used (see
`kernel/module/main.c:3117`, confirmed above), so there's no new frame
here to find. What's worth actually looking at is frame `#1`'s own
`fn=` argument: GDB may print it as `<init_module>` rather than
`<my_init>` even though you broke on `my_init` and frame `#0` shows
`my_init` — both names really do point at the identical address, and
which one a given piece of output picks is just which symbol GDB
happened to prefer for that address, not a sign anything is wrong.
`print/x $pc` in frame 0 and `info symbol $pc` will show you both names
resolve to the same value if you want to confirm it directly.

```gdb
(gdb) finish
Run till exit from #0  my_init () at better_hello.c:16
0x... in do_one_initcall (fn=0x... <init_module>) at init/main.c:...
Value returned is $1 = 0
```

Exit path, same shape as module 01:

```gdb
(gdb) break my_exit
(gdb) continue
```
```bash
# vmb:
rmmod better_hello
```
```
Thread 2 hit Breakpoint 3, my_exit () at better_hello.c:21
21          printk(KERN_INFO "bye bye my luv\n");
(gdb) lx-dmesg
```

## Cleanup

```gdb
(gdb) delete
```
```bash
# vmb:
poweroff -f
```

## What this proves

`module_init()`/`module_exit()` are alias-generating macros, not a
different call mechanism — `nm`'s two symbols at one address is direct,
verifiable proof, and the identical `do_one_initcall()` → `mod->init`
call path (confirmed straight from `kernel/module/main.c`) shows the
runtime behavior really is the same as module 01's. The one genuine
payoff is being able to `break my_init` by the name that actually
appears in the source, instead of every module in the world sharing
the one generic name `init_module` the way module 01's did. Every
module from here on uses this modern pattern and its real function
names; this is the one that establishes why that's safe to rely on.
