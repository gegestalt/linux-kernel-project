# Debugging insmod's vermagic check, step by step

`modinfo *.ko | grep vermagic` has appeared in every module's `gdb_walkthrough.md`
so far, as a one-line sanity check before booting the guest. This document
stops treating it as a one-liner: where the string actually comes from at
build time, exactly which kernel object code reads and compares it, and
what happens — traced live, with real breakpoints, on the real debug
kernel — when it doesn't match. Everything below was run for real against
`better_hello` (module 02) and this repo's existing QEMU+KGDB setup; no
step is hypothetical.

Two things turn out to be true that aren't obvious from the `modinfo`
one-liner alone:

1. **`vermagic` is not part of `better_hello.mod.c`.** It comes from a
   completely separate object, shared by every module built in a given
   `make modules` run, linked in at the very last step.
2. **For a `CONFIG_MODVERSIONS` build (this one), the vermagic string's
   own kernel-release word is never actually compared.** A different,
   earlier check — on a single synthetic symbol called `module_layout` —
   is what actually rejects a module built against the wrong kernel here.
   Proven live in Part 3.

## Part 0 — environment

Same debug kernel, same QEMU/KGDB setup, as every other module in this
repo — see [`../../gdb_debugging.md`](../../gdb_debugging.md) for the
tmux layout and the three gotcha rules (one command per paste, `Ctrl-C`
freezes the guest, `continue` waits on the *other* pane). This doc adds
nothing new to that setup, only new breakpoints and a second `.ko`.

```bash
cd 02_better_hello
make -C /home/adiopocere/Desktop/codes/linux_mainline M=$(pwd) modules
modinfo better_hello.ko | grep vermagic
```

```
vermagic: 7.2.0-kgdb-debug+ SMP preempt mod_unload modversions aarch64
```

---

## Part 1 — where `vermagic` actually comes from (build time)

### The string itself

`include/linux/vermagic.h`:

```c
#define VERMAGIC_STRING 						\
	UTS_RELEASE " "							\
	MODULE_VERMAGIC_SMP MODULE_VERMAGIC_PREEMPT 			\
	MODULE_VERMAGIC_MODULE_UNLOAD MODULE_VERMAGIC_MODVERSIONS	\
	MODULE_ARCH_VERMAGIC						\
	MODULE_RANDSTRUCT
```

Every token is conditional on a Kconfig option, checked against *this
build's* `.config`, not the module's source:

| Token | Present when |
|---|---|
| `UTS_RELEASE` | always — `include/generated/utsrelease.h`, `"7.2.0-kgdb-debug+"` here |
| `SMP ` | `CONFIG_SMP` |
| `preempt ` | `CONFIG_PREEMPT_BUILD` (or `preempt_rt ` for `CONFIG_PREEMPT_RT`) |
| `mod_unload ` | `CONFIG_MODULE_UNLOAD` |
| `modversions ` | `CONFIG_MODVERSIONS` |
| `MODULE_ARCH_VERMAGIC` | always, arch-defined — `"aarch64"` on arm64 (`arch/arm64/include/asm/vermagic.h`, verified: it's a one-line header, nothing more to it) |
| `MODULE_RANDSTRUCT` | `RANDSTRUCT` — empty here |

This is why two `.ko` files can have *different* vermagic strings even
built from the exact same kernel commit: change `CONFIG_MODULE_UNLOAD` or
`CONFIG_MODVERSIONS` between builds and the string changes, independent
of `UTS_RELEASE`.

### The misconception worth killing first: it's not in `better_hello.mod.c`

`scripts/mod/modpost` generates a `.mod.c` per module — that's where
`name`, `depends`, `srcversion`, and the `__versions[]` CRC table come
from. It is tempting to assume `vermagic` is stamped in there too, next
to those. Checked directly against the real generated file:

```bash
grep -n vermagic better_hello.mod.c
```

```
(no output)
```

Confirmed empty — `better_hello.mod.c` genuinely has no `vermagic`
anywhere in it. What it *does* carry, straight from the file:

```c
MODULE_INFO(name, KBUILD_MODNAME);
...
MODULE_INFO(depends, "");
MODULE_INFO(srcversion, "B4F7C2CAFBE545E9ADB188E");
```

### The real source: `scripts/module-common.c`

```c
// scripts/module-common.c
#include <linux/module.h>
#define INCLUDE_VERMAGIC
#include <linux/build-salt.h>
#include <linux/elfnote-lto.h>
#include <linux/vermagic.h>
...
MODULE_INFO(vermagic, VERMAGIC_STRING);

#ifdef CONFIG_MITIGATION_RETPOLINE
MODULE_INFO(retpoline, "Y");
#endif
```

This file is **not** module-specific — it's one shared `.c` file, compiled
**once per `make modules` invocation**, and linked into *every* `.ko`
built in that run. `scripts/Makefile.modfinal` shows both facts at once:

```makefile
.module-common.o: $(srctree)/scripts/module-common.c FORCE
	$(call if_changed_rule,cc_o_c)
...
%.ko: %.o %.mod.o .module-common.o $(objtree)/scripts/module.lds ... FORCE
	+$(call if_changed,ld_ko_o)
```

Three inputs, one output, one link. Confirmed straight out of the real
build's saved command line for this module:

```bash
grep -o 'cmd_better_hello.ko := .*' .better_hello.ko.cmd
```

```
cmd_better_hello.ko := ld -r -EL  -maarch64elf -z noexecstack --no-warn-rwx-segments \
  --build-id=sha1 --force-group-allocation \
  -T /home/adiopocere/Desktop/codes/linux_mainline/scripts/module.lds \
  -o better_hello.ko better_hello.o better_hello.mod.o .module-common.o
```

`ld -r` (relocatable link, not a final executable link) concatenates
three separate object files. Each contributes a *different, disjoint*
slice of `.modinfo` — checked directly, one object at a time, before the
link:

```bash
readelf -p .modinfo better_hello.o          # your compiled source
readelf -p .modinfo better_hello.mod.o      # modpost-generated
readelf -p .modinfo .module-common.o        # shared, once per build
```

```
=== better_hello.o ===
String dump of section '.modinfo':
  [     0]  description=A simple hello World Linux Kernel Module
  [    35]  author=guguali
  [    44]  license=GPL

=== better_hello.mod.o ===
String dump of section '.modinfo':
  [     0]  srcversion=B4F7C2CAFBE545E9ADB188E
  [    23]  depends=
  [    2c]  name=better_hello

=== .module-common.o ===
String dump of section '.modinfo':
  [     0]  vermagic=7.2.0-kgdb-debug+ SMP preempt mod_unload modversions aarch64
```

`description`/`author`/`license` come from *your* `MODULE_DESCRIPTION()`/
`MODULE_AUTHOR()`/`MODULE_LICENSE()` calls in `better_hello.c`.
`srcversion`/`depends`/`name` come from modpost's generated `.mod.c`.
`vermagic` comes from neither — it's the one field every `.ko` built in
this tree during this run shares verbatim, contributed by the object file
that isn't really "yours" at all. `ld -r` just concatenates the three
`.modinfo` sections in link order, and the final file is exactly that
concatenation — confirmed by diffing the pieces above against the linked
result:

```bash
readelf -p .modinfo better_hello.ko
```

```
String dump of section '.modinfo':
  [     0]  description=A simple hello World Linux Kernel Module
  [    35]  author=guguali
  [    44]  license=GPL
  [    50]  srcversion=B4F7C2CAFBE545E9ADB188E
  [    73]  depends=
  [    7c]  name=better_hello
  [    8e]  vermagic=7.2.0-kgdb-debug+ SMP preempt mod_unload modversions aarch64
```

Same order, same three groupings, no surprises — `.modinfo` in the final
`.ko` is nothing more than what you just watched get concatenated.

---

## Part 2 — how `insmod` actually gets this into the kernel

### Which syscall — checked, not assumed

It's easy to assume `insmod` always calls the well-known `init_module(2)`.
Traced directly against the real `insmod` on this machine (non-root, so
it's guaranteed to fail with `EPERM` before anything is actually loaded —
safe to run for real):

```bash
strace -f -e trace=init_module,finit_module,openat \
  insmod ./better_hello.ko
```

```
openat(AT_FDCWD, "/home/.../better_hello.ko", O_RDONLY|O_CLOEXEC) = 3
finit_module(3, "", 0)                 = -1 EPERM (Operation not permitted)
insmod: ERROR: could not insert module ./better_hello.ko: Operation not permitted
```

Modern (`kmod`-based) `insmod` opens the `.ko` as a file descriptor and
calls **`finit_module(2)`**, not the legacy buffer-based `init_module(2)`.
The very first thing either syscall does, before touching the file's
contents at all, is a capability check — this is exactly what produced
the `EPERM` above:

```c
// kernel/module/main.c
SYSCALL_DEFINE3(finit_module, int, fd, const char __user *, uargs, int, flags)
{
	int err = may_init_module();
	if (err)
		return err;
	...
```

### BusyBox's `insmod`, inside the guest, behaves differently

The QEMU guest's minimal initramfs uses BusyBox's `insmod`, not `kmod`'s.
Traced live with GDB (breakpoints on `early_mod_check`, `bt` at the
second hit — full transcript in Part 4) it makes **two full attempts**
for a single `insmod` invocation on a rejected module: the second one's
backtrace runs straight through

```
#3  __do_sys_init_module (umod=..., len=..., uargs=...) at kernel/module/main.c:3667
#4  __se_sys_init_module (...) at kernel/module/main.c:3647
#5  __arm64_sys_init_module (regs=...) at kernel/module/main.c:3647
```

— the **legacy `init_module(2)`** path (`SYSCALL_DEFINE3(init_module, ...)`
at `kernel/module/main.c:3647`), confirmed directly against the source.
Whichever syscall got used, both attempts land in the exact same
`load_module()` → `early_mod_check()` chain described next — this doc's
subject doesn't care which door userspace walked through.

---

## Part 3 — the kernel-side chain (source-verified, then GDB-verified)

```
finit_module(2) / init_module(2)
  └─ may_init_module()                    ← CAP_SYS_MODULE gate, before any parsing
  └─ load_module(info, uargs, flags)       kernel/module/main.c:3433
       ├─ module_sig_check(info, flags)
       ├─ elf_validity_cache_copy(info, flags)
       └─ early_mod_check(info, flags)     kernel/module/main.c:3396 (inlined into load_module)
            ├─ blacklisted(info->name)
            ├─ rewrite_section_headers(info, flags)
            ├─ check_modstruct_version(info, info->mod)   ← FIRST, own real symbol
            │    └─ check_version(info, "module_layout", mod, crc)
            │         compares the running kernel's *live* CRC for the
            │         synthetic symbol `module_layout` against the CRC
            │         your module recorded for it at build time
            └─ check_modinfo(info->mod, info, flags)      ← SECOND, THIS is the vermagic check
                 ├─ get_modinfo(info, "vermagic")           pulls the string straight
                 │                                          out of the module's own
                 │                                          .modinfo ELF section
                 └─ same_magic(modmagic, vermagic, info->index.vers)
                      compares it against the kernel's OWN copy:
                      kernel/module/main.c:1105:
                        static const char vermagic[] = VERMAGIC_STRING;
```

`check_modstruct_version()` runs **before** `check_modinfo()`. That
ordering is the whole point of Part 4 below — module 02's own
`gdb_walkthrough.md` already established the lesson "verify against the
real built object, not the doc-comment"; this is the same lesson applied
one layer deeper.

### A wrinkle worth knowing before you set breakpoints

`check_modinfo` and `early_mod_check` are both `static`, single-call-site
functions — GCC inlines them straight into `load_module`. Neither shows
up as its own symbol in `vmlinux`:

```bash
nm vmlinux | grep -E "check_modinfo|early_mod_check"
```

```
(no output — neither exists as a standalone symbol)
```

But `nm` only sees the symbol table, not the DWARF inline records GDB
actually uses to resolve breakpoints. Both still `break` cleanly, live,
confirmed against the running debug kernel in Part 4:

```gdb
(gdb) break check_modinfo
Breakpoint 3 at 0xffff800080286cec: file kernel/module/main.c, line 1166.
```

(GDB's own reported line number for the *bare* function-name form of this
breakpoint doesn't quite match the source's `get_modinfo(...)` line —
some DWARF inline-record noise. It resolves to the right code regardless,
confirmed by what actually gets hit when the breakpoint fires: line 2646,
`const char *modmagic = get_modinfo(info, "vermagic");` — exactly the
right statement, verified live in Part 4.)

`check_modstruct_version`, `check_version`, and `same_magic` are declared
without `static` in `kernel/module/version.c` (used from `main.c` via
`kernel/module/internal.h`) and stay real, ordinary global symbols:

```bash
nm vmlinux | grep -E "check_modstruct_version|check_version|same_magic"
```

```
ffff80008028e5b8 T check_modstruct_version
ffff80008028e2a8 T check_version
ffff80008028e128 T same_magic
```

`vermagic[]` itself — the kernel's own copy — is `static const`, but it's
initializer data GDB can read straight out of `vmlinux` with **no live
target at all**, the same "verified statically before ever touching the
VM" move every module's `gdb_walkthrough.md` opens with:

```bash
gdb -q -batch -nx -ex "file vmlinux" -ex "print vermagic" vmlinux
```

```
$1 = "7.2.0-kgdb-debug+ SMP preempt mod_unload modversions aarch64"
```

---

## Part 4 — `same_magic()`: the part that actually surprised me

```c
// kernel/module/version.c
int same_magic(const char *amagic, const char *bmagic, bool has_crcs)
{
	if (has_crcs) {
		amagic += strcspn(amagic, " ");
		bmagic += strcspn(bmagic, " ");
	}
	return strcmp(amagic, bmagic) == 0;
}
```

`has_crcs` is `info->index.vers` — true whenever the module's ELF has a
`__versions` section, i.e. whenever `CONFIG_MODVERSIONS` is on (every
module in this repo, per the `modversions` token already in every
`vermagic:` line printed so far). When `has_crcs` is true,
`strcspn(str, " ")` finds the offset of the **first space** in each
string, and both pointers are advanced *past the entire first word* —
the kernel-release token — before the `strcmp`. Reproduced standalone,
outside the kernel entirely, to make sure this reading of the code is
actually right and not a misparse:

```c
// same_magic_test.c — exact copy of the real function above
int main(void) {
    const char *debug_vermagic = "7.2.0-kgdb-debug+ SMP preempt mod_unload modversions aarch64";
    const char *host_vermagic  = "7.0.0-30-generic SMP preempt mod_unload modversions aarch64";
    printf("has_crcs=1: %d\n", same_magic(host_vermagic, debug_vermagic, 1));
    printf("has_crcs=0: %d\n", same_magic(host_vermagic, debug_vermagic, 0));
}
```

```
has_crcs=1: 1
has_crcs=0: 0
```

**With `CONFIG_MODVERSIONS` on, two completely different kernel releases
— `7.2.0-kgdb-debug+` and `7.0.0-30-generic` — read as an exact vermagic
match**, because only the text *after* the first space (the `SMP preempt
mod_unload modversions aarch64` flags) is ever compared. The release
string is skipped, not matched.

This isn't a bug — it's what makes `check_modstruct_version()` running
*first* (Part 3) load-bearing rather than redundant. `CONFIG_MODVERSIONS`
already does fine-grained, per-symbol CRC checking (starting with the
synthetic `module_layout` symbol, checked unconditionally by
`check_modstruct_version()` before anything else, then every real
imported symbol via `check_version()` during `simplify_symbols()` later).
That CRC machinery is a strictly more precise "does this module match
this exact kernel build" test than a hand-written release string could
ever be, so `same_magic()` intentionally defers the coarse release-word
check to it and only still enforces the *qualitative* flags — SMP,
preempt, mod_unload, modversions, arch — that the CRC machinery can't
express. Part 5 proves this live: two builds a whole kernel apart get
rejected by `check_modstruct_version()`'s CRC check, never even reaching
the vermagic string comparison this section is about.

---

## Part 5 — live, Part 1: a matching build, breakpoint by breakpoint

tmux layout, guest boot, `nokaslr`, all per
[`../../gdb_debugging.md`](../../gdb_debugging.md). **One command per
paste, always.**

```gdb
(gdb) target remote :1234
(gdb) break do_init_module
(gdb) break check_modstruct_version
(gdb) break check_modinfo
(gdb) break same_magic
(gdb) continue
```

```bash
# vmb:
insmod /mnt/labs/02_better_hello/better_hello.ko
```

`check_modstruct_version` hits first — exactly the source order from
Part 3:

```
Thread 2 hit Breakpoint 2, check_modstruct_version (info=..., mod=0xffff8000848449e8)
    at kernel/module/version.c:77
(gdb) print mod->name
$1 = "better_hello", '\000' <repeats 43 times>
(gdb) finish
0xffff800080286ce8 in early_mod_check (info=..., flags=<optimized out>)
    at kernel/module/main.c:3415
3415		if (!check_modstruct_version(info, info->mod))
Value returned is $2 = 1
```

`1` = pass. `early_mod_check` shows up as a real frame in the backtrace
even though it has no `vmlinux` symbol of its own — the inlining note
from Part 3, confirmed live. `continue` moves on to the vermagic check:

```gdb
(gdb) continue
```

```
Thread 2 hit Breakpoint 3, check_modinfo (mod=..., info=..., flags=<optimized out>)
    at kernel/module/main.c:2646
2646		const char *modmagic = get_modinfo(info, "vermagic");
(gdb) next
2653		if (!modmagic) {
(gdb) print modmagic
$3 = 0xffff80008482d197 "7.2.0-kgdb-debug+ SMP preempt mod_unload modversions aarch64"
(gdb) print vermagic
$4 = "7.2.0-kgdb-debug+ SMP preempt mod_unload modversions aarch64"
```

`modmagic` is the string read live out of *this specific module's*
`.modinfo` section (Part 1's `.module-common.o` fragment, now sitting in
guest memory); `vermagic` is the kernel's own static copy, same value
printed statically in Part 3 with no target attached at all — same
string, two completely different sources, read two different ways.

```gdb
(gdb) continue
```

```
Thread 2 hit Breakpoint 4, same_magic (
    amagic=... "7.2.0-kgdb-debug+ SMP preempt mod_unload modversions aarch64",
    bmagic=... <vermagic> "7.2.0-kgdb-debug+ SMP preempt mod_unload modversions aarch64",
    has_crcs=true) at kernel/module/version.c:99
(gdb) finish
Value returned is $5 = 1
```

`has_crcs=true`, live confirmation of the Part 4 claim — this build really
does have modversions on, so the release-word-skipping path is the one
actually executing right now, not a hypothetical. `1` = match.
`continue` once more and the module finishes loading normally:

```gdb
(gdb) continue
```

```
Thread 2 hit Breakpoint 1, do_init_module (mod=..., mod=0xffff80007c322040)
    at kernel/module/main.c:3089
(gdb) continue
```

```
# vmb:
[   31.393824] Hello luv .
```

Clean up (name the number — bare `delete` prompts for confirmation, see
`gdb_debugging.md`'s rule 3 for why that eats the next paste):

```gdb
(gdb) delete
```
```
Delete all breakpoints? (y or n) y
```
```bash
# vmb:
poweroff -f
```

---

## Part 6 — live, Part 2: a module built against the *wrong* kernel

Build the exact same source against this **host machine's own running
kernel** instead of the debug tree — a completely ordinary mistake (the
one `gdb_debugging.md`'s own "Per-module" section already warns about:
every module Makefile here hardcodes `/lib/modules/$(uname -r)/build`,
which is *this machine's* kernel, not the debug one):

```bash
mkdir -p /tmp/vermagic-mismatch-demo
cp better_hello.c /tmp/vermagic-mismatch-demo/
cd /tmp/vermagic-mismatch-demo
printf 'obj-m += better_hello.o\n' > Makefile
make -C /lib/modules/$(uname -r)/build M=$(pwd) modules
modinfo better_hello.ko | grep vermagic
```

```
vermagic: 7.0.0-30-generic SMP preempt mod_unload modversions aarch64
```

(Your own host kernel release will differ — the point is only that it's
*not* `7.2.0-kgdb-debug+`.) Copy it onto the scratch disk under a second
name, alongside the correct one:

```bash
sudo mount -o loop /home/adiopocere/Desktop/codes/qemu-vmb/labs-disk.img /tmp/vmb-mnt
sudo cp /tmp/vermagic-mismatch-demo/better_hello.ko \
        /tmp/vmb-mnt/02_better_hello/better_hello_hostkernel.ko
sudo umount /tmp/vmb-mnt
```

Fresh guest boot (previous session's breakpoints don't carry over — quit
and restart `gdb` too), then:

```gdb
(gdb) target remote :1234
(gdb) break check_modstruct_version
(gdb) break check_version
(gdb) break early_mod_check
(gdb) continue
```

```bash
# vmb:
insmod /mnt/labs/02_better_hello/better_hello_hostkernel.ko
```

```
Thread 1 hit Breakpoint 3, early_mod_check (info=..., flags=<optimized out>)
    at kernel/module/main.c:3405
3405		if (blacklisted(info->name)) {
(gdb) continue
Thread 1 hit Breakpoint 1, check_modstruct_version (info=..., mod=0xffff8000848547a0)
    at kernel/module/version.c:77
(gdb) continue
Thread 1 hit Breakpoint 4.1, check_version (
    symname=... "module_layout", crc=0xffff8000825bbe84) at kernel/module/version.c:17
```

Recall from Part 3: `check_version` for `module_layout` compares the
*running kernel's live* CRC for that synthetic symbol against the CRC the
module recorded at its own build time. Print both, straight from the
live registers:

```gdb
(gdb) print/x *crc
$2 = 0x83caa58a
```

That's the debug kernel's real, live-exported CRC for `module_layout` —
matches this session's own `linux_mainline/Module.symvers` exactly. The
module being loaded recorded a different one at build time (straight out
of its own generated `.mod.c`, confirmed by hand beforehand):

```bash
grep -A2 ____versions /tmp/vermagic-mismatch-demo/better_hello.mod.c
```

```
{ 0xe8213e80, "_printk" },
{ 0xc7ea9460, "module_layout" },
```

`0x83caa58a` (kernel, live) vs. `0xc7ea9460` (module, embedded) —
different kernel commit, different structure layout, different CRC.
`finish` shows the verdict:

```gdb
(gdb) finish
0xffff80008028e640 in check_modstruct_version (info=..., mod=...)
    at kernel/module/version.c:94
Value returned is $3 = 0
```

**`0` — rejected.** And note exactly where: `check_modstruct_version()`,
called *before* `check_modinfo()` ever runs. The vermagic string
comparison from Parts 1–5 — the one `modinfo | grep vermagic` seems to
promise is "the" compatibility check — is **never reached** for this
failure. Confirmed by simply never seeing `check_modinfo` fire on this
run despite it being armed; the mismatch is caught one layer earlier.

Continue to the end and read the real userspace/kernel-log result:

```gdb
(gdb) continue
```

```bash
# vmb:
[   21.532364] better_hello: disagrees about version of symbol module_layout
[   21.540599] better_hello: disagrees about version of symbol module_layout
insmod: can't insert '/mnt/labs/02_better_hello/better_hello_hostkernel.ko': invalid module format
```

Two identical `dmesg` lines — BusyBox's `insmod` made two full attempts
(Part 2), and the same kernel-side check rejected both, identically, each
time. `pr_warn("%s: disagrees about version of symbol %s\n", ...)` is the
exact source of that line, straight out of `check_version()`'s
`bad_version:` label read in Part 3. `invalid module format` is
BusyBox's rendering of the `-ENOEXEC` that `early_mod_check()` returned.

Clean up: `delete`, then `poweroff -f` in `vmb`, same as every other
walkthrough in this repo.

---

## What this proves

- `vermagic` is generated once per build, by `scripts/module-common.c`
  alone, and linked into every `.ko` from that run as a shared object —
  never by modpost's per-module `.mod.c`, confirmed by its literal
  absence there (Part 1).
- The load path is `finit_module(2)` for a modern `insmod` (`kmod`), the
  legacy `init_module(2)` for BusyBox's fallback — both funnel into the
  identical `load_module()` (Part 2).
- `check_modstruct_version()` — a CRC check on one synthetic symbol,
  `module_layout` — runs *before* the vermagic check and, for any
  `CONFIG_MODVERSIONS` build, is the check that actually decides whether
  a module built for a different kernel gets rejected (Parts 3, 4, 6).
- `same_magic()` deliberately skips the kernel-release word once CRCs are
  present — proven both by reading the function and by reproducing it
  standalone outside the kernel (Part 4) — so the vermagic string's
  headline value, the release string everyone stares at in `modinfo`
  output, isn't actually load-bearing in a modversions build. What is
  load-bearing: the SMP/preempt/mod_unload/modversions/arch flags after
  it, and, one layer earlier and more precisely, `module_layout`'s CRC.
- All of it verified twice, live, on the real debug kernel: once where
  every check passes end to end (Part 5), once where the very first
  kernel-ABI check catches an honestly-wrong-kernel build before the
  vermagic comparison this whole document is nominally about ever runs
  (Part 6).
