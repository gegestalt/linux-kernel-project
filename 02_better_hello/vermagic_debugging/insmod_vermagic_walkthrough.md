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
   Proven live in Part 7.

## Part 0 — environment

Same debug kernel, same QEMU/KGDB setup, as every other module in this
repo — see [`../../gdb_debugging.md`](../../gdb_debugging.md) for the
tmux layout and the four gotcha rules (one command per paste, `Ctrl-C`
freezes the guest, `continue` waits on the *other* pane, and — if you're
scripting this rather than typing it by hand — `tmux send-keys`'s own
key-name gotcha). This doc adds nothing new to that setup, only new
breakpoints and a second `.ko`.

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
second hit) it makes **two full attempts** for a single `insmod`
invocation on a rejected module (full walkthrough in Part 7): the second
one's backtrace runs straight through

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
ordering is the whole point of Part 5 below — module 02's own
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
confirmed against the running debug kernel in Part 6:

```gdb
(gdb) break check_modinfo
Breakpoint 3 at 0xffff800080286cec: file kernel/module/main.c, line 1166.
```

(GDB's own reported line number for the *bare* function-name form of this
breakpoint doesn't quite match the source's `get_modinfo(...)` line —
some DWARF inline-record noise. It resolves to the right code regardless,
confirmed by what actually gets hit when the breakpoint fires: line 2646,
`const char *modmagic = get_modinfo(info, "vermagic");` — exactly the
right statement, verified live in Part 6.)

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

## Part 4 — every parameter, traced: what goes in, what it's compared against

Part 3 gave the call chain's shape. This part opens every frame in it and
answers, argument by argument: what does this function actually receive,
where did the caller get that value, and what specific *other* value does
it get compared against to decide pass or fail. All live values below are
from the same kind of QEMU/KGDB session as Part 6 — `better_hello.ko`
loading cleanly into the debug kernel — captured with breakpoints on
`check_modstruct_version`, `find_symbol`, and `check_modinfo`.

### 4.1 — `struct load_info *info`: the one parameter everything else derives from

Every function in this chain takes `info` (or something derived from it)
as its first real argument. It's built once, early in `load_module()`
(`elf_validity_cache_copy()`), and never rebuilt — the same struct
instance flows all the way down. Its full definition,
`kernel/module/internal.h`:

```c
struct load_info {
	const char *name;
	struct module *mod;
	Elf_Ehdr *hdr;
	unsigned long len;
	Elf_Shdr *sechdrs;
	char *secstrings, *strtab;
	unsigned long symoffs, stroffs, init_typeoffs, core_typeoffs;
	bool sig_ok;
	...
	struct {
		unsigned int sym;
		unsigned int str;
		unsigned int mod;
		unsigned int vers;
		unsigned int info;
		unsigned int pcpu;
		unsigned int vers_ext_crc;
		unsigned int vers_ext_name;
	} index;
};
```

The fields this whole document's chain actually touches:

| Field | What it holds | Where it's set |
|---|---|---|
| `hdr` | pointer to the raw, unmodified `.ko` file bytes, copied into kernel memory | `elf_validity_cache_copy()` |
| `sechdrs` | the module's own ELF section header table (a copy, patched in place as loading proceeds) | same |
| `index.info` | section-header-table **index** of the module's `.modinfo` section | `elf_validity_cache_index_info()`, via `find_sec(info, ".modinfo")` |
| `index.vers` | section-header-table index of the module's `__versions` section — **0 if absent** | `elf_validity_cache_index_versions()`, via `find_sec(info, "__versions")` |
| `name` | the module's own name, itself read out of `.modinfo`'s `name=` tag | `info->name = get_modinfo(info, "name");` |
| `mod` | pointer to the (still-forming) `struct module` for this load | allocated in `layout_and_allocate()` |

Live, at the `check_modstruct_version` breakpoint, for `better_hello.ko`:

```gdb
(gdb) print info->index.vers
$1 = 41
(gdb) print info->index.info
$2 = 20
(gdb) print info->name
$3 = 0xffff80008483d181 "better_hello"
```

`41` and `20` are real ELF section-header indices *inside this specific
`.ko`* — not magic constants. `20` matches this module's own `.ko` file
exactly: `readelf -S better_hello.ko` (Part 1's own build) reports
`.modinfo` as section `[20]`. `info->index.vers` being non-zero (`41`,
not `0`) is itself the entire input to `has_crcs` used throughout this
document — confirmed by reading `elf_validity_cache_index_versions()`
directly:

```c
// kernel/module/main.c
static int elf_validity_cache_index_versions(struct load_info *info, int flags)
{
	/* If modversions were suppressed, pretend we didn't find any */
	if (flags & MODULE_INIT_IGNORE_MODVERSIONS) {
		info->index.vers = 0;
		...
		return 0;
	}
	...
	info->index.vers = find_sec(info, "__versions");
	...
}
```

Two ways `info->index.vers` ends up `0` (no CRCs, full-string vermagic
compare — Part 5): the module genuinely has no `__versions` section
(built with `CONFIG_MODVERSIONS=n`), **or** the caller passed the
`MODULE_INIT_IGNORE_MODVERSIONS` flag to `finit_module(2)`/`init_module(2)`
and the kernel forces it to `0` regardless of what the module's ELF
actually contains. That flag is exactly what `modprobe --force-modversion`
(confirmed against `modprobe --help` on this machine: `--force-modversion
Ignore module's version`) sets on your behalf — the userspace flag name
and the kernel-side flag name aren't obviously the same word, but they're
the same knob. `modprobe --force-vermagic` is the separate, corresponding
knob for `MODULE_INIT_IGNORE_VERMAGIC` in Part 4.4; plain `-f`/`--force`
sets both at once. This is decided entirely at the syscall-flag level,
before `same_magic()` is ever reached — not something it decides on its
own.

Also worth confirming directly: `info->name` above (`0xffff80008483d181`)
is a pointer straight into the module's own raw `.modinfo` bytes — not a
separate allocation. Proven by address arithmetic against Part 4.4's live
`.modinfo` dump below: the `name=better_hello` tag sits at
`0xffff80008483d17c`; `"name="` is 5 bytes; `0xffff80008483d17c + 5 =
0xffff80008483d181` — exactly `info->name`'s address. `info->name =
get_modinfo(info, "name")` (`kernel/module/main.c:2105`) really is just a
raw pointer *into* the file, confirmed by the arithmetic matching
byte-for-byte.

### 4.2 — `check_modstruct_version(info, mod)`: what it actually delegates to

```c
// kernel/module/version.c
int check_modstruct_version(const struct load_info *info, struct module *mod)
{
	struct find_symbol_arg fsa = {
		.name	= "module_layout",
		.gplok	= true,
	};
	bool have_symbol;

	scoped_guard(rcu)
		have_symbol = find_symbol(&fsa);
	BUG_ON(!have_symbol);

	return check_version(info, "module_layout", mod, fsa.crc);
}
```

Its own two parameters barely get used directly — `info` is just handed
straight through to `check_version()`, `mod` only ever reaches a
`pr_warn()`/`pr_debug()` message on failure. The real work is the local
`struct find_symbol_arg fsa`, built fresh on the stack every single call,
always asking for one hardcoded name: `"module_layout"`.

`struct find_symbol_arg`, `kernel/module/internal.h` — an explicit
input/output split:

```c
struct find_symbol_arg {
	/* Input */
	const char *name;
	bool gplok;
	bool warn;

	/* Output */
	struct module *owner;
	const u32 *crc;
	const struct kernel_symbol *sym;
	enum mod_license license;
};
```

Live, printed at entry to `find_symbol()` — only the input fields are
meaningful yet:

```gdb
(gdb) print *fsa
$4 = {name = 0xffff8000822d4d90 "module_layout", gplok = true, warn = false,
      owner = 0x0, crc = 0x0, sym = 0x0, license = NOT_GPL_ONLY}
```

`finish` out of `find_symbol()`, then print the *same* struct again from
`check_modstruct_version()`'s own frame:

```gdb
(gdb) finish
Value returned is $5 = true
(gdb) print fsa
$6 = {name = 0xffff8000822d4d90 "module_layout", gplok = true, warn = false,
      owner = 0x0, crc = 0xffff8000825bbe84, sym = 0xffff8000825a1824,
      license = NOT_GPL_ONLY}
```

`owner` is *still* `0x0` after a successful lookup — not a bug, a fact
worth knowing: `module_layout` is exported by `vmlinux` itself
(`kernel/module/version.c`'s own `EXPORT_SYMBOL(module_layout);`), and
`find_symbol()` represents "owned by the kernel image, not a module" as a
`NULL` owner. `crc` now points at a real address — dereferenced live:

```gdb
(gdb) print/x *fsa.crc
$7 = 0x83caa58a
```

Where that pointer actually comes from, read straight out of
`find_symbol()`/`find_exported_symbol_in_section()`:

```c
// kernel/module/main.c
bool find_symbol(struct find_symbol_arg *fsa)
{
	const struct symsearch syms = {
		.start = __start___ksymtab, .stop = __stop___ksymtab,
		.crcs  = __start___kcrctab, .flagstab = __start___kflagstab,
	};
	if (find_exported_symbol_in_section(&syms, NULL, fsa))
		return true;
	list_for_each_entry_rcu(mod, &modules, list, ...) {
		/* same search, but over mod->syms / mod->crcs instead */
		...
	}
	...
}

static bool find_exported_symbol_in_section(const struct symsearch *syms,
					    struct module *owner,
					    struct find_symbol_arg *fsa)
{
	sym = bsearch(fsa->name, syms->start, syms->stop - syms->start,
		      sizeof(struct kernel_symbol), cmp_name);
	if (!sym)
		return false;
	...
	fsa->crc = symversion(syms->crcs, sym - syms->start);
	...
}
```

`__start___ksymtab`/`__stop___ksymtab` bound `vmlinux`'s own sorted table
of every `EXPORT_SYMBOL()`'d name; `bsearch()` finds `"module_layout"` in
it by binary search; `__start___kcrctab` is a **second array, running
parallel to the first** — same index, one CRC per exported symbol —  so
`symversion(syms->crcs, sym - syms->start)` just indexes straight across
from the matched name to its CRC. This is exactly `fsa->crc`'s origin:
**not** a value baked into any module, but the *currently running
kernel's own, live, just-computed* export table entry for that name.
`module_layout` itself, `kernel/module/version.c`:

```c
void module_layout(struct module *mod, struct modversion_info *ver,
		    struct kernel_param *kp, struct kernel_symbol *ks,
		    struct tracepoint * const *tp) { }
EXPORT_SYMBOL(module_layout);
```

An empty function that exists purely so its *parameter types* have
something to attach a CRC to. modversioning computes each exported
symbol's CRC from the C types visible in its declaration — so this one
symbol's CRC is really a fingerprint over `struct module`, `struct
modversion_info`, `struct kernel_param`, `struct kernel_symbol`, and
`struct tracepoint`'s layouts all at once. Any one of those changing
between kernel builds changes this single CRC — which is exactly why
comparing it is a strictly stronger "is this the right kernel" test than
comparing a hand-written release string (Part 5 gets to why that matters).

### 4.3 — `check_version(info, symname, mod, crc)`: the same comparator, reused for every symbol

```c
// kernel/module/version.c
int check_version(const struct load_info *info, const char *symname,
		   struct module *mod, const u32 *crc)
{
	Elf_Shdr *sechdrs = info->sechdrs;
	unsigned int versindex = info->index.vers;
	struct modversion_info *versions;
	unsigned int i, num_versions;

	if (!crc)
		return 1;                          /* exporter untracked — already tainted */
	if (versindex == 0)
		return try_to_force_load(mod, symname) == 0;

	versions = (void *)sechdrs[versindex].sh_addr;
	num_versions = sechdrs[versindex].sh_size / sizeof(*versions);

	for (i = 0; i < num_versions; i++) {
		if (strcmp(versions[i].name, symname) != 0)
			continue;
		if (versions[i].crc == *crc)
			return 1;
		goto bad_version;
	}
	return 1;                                  /* symbol just isn't versioned */

bad_version:
	pr_warn("%s: disagrees about version of symbol %s\n", info->name, symname);
	return 0;
}
```

Four parameters, four distinct roles — none of them redundant:

| Param | Role | Value, live (`module_layout` case) |
|---|---|---|
| `info` | locate **this module's own** recorded CRC table (`info->sechdrs[info->index.vers]`) and its name for the warning | the same `info` from 4.1 |
| `symname` | which entry to look up in that table | `"module_layout"` (hardcoded caller in 4.2) or, generically, any imported symbol's name |
| `mod` | only used by `try_to_force_load()` if `versindex == 0` | `better_hello`'s (still-forming) `struct module` |
| `crc` | the comparison target — the **currently running kernel's live CRC** for `symname`, already resolved by `find_symbol()` in 4.2 | `fsa.crc`, dereferences to `0x83caa58a` |

The comparison itself, spelled out: `*crc` (kernel, live, right now, from
4.2) against `versions[i].crc` — one entry out of *this module's own*
`__versions` ELF section, matched by `symname` via linear scan. That
table is exactly the `____versions[]` array modpost generated back in
Part 1 — read straight from `better_hello.mod.c`:

```c
static const struct modversion_info ____versions[]
__used __section("__versions") = {
	{ 0x92997ed8, "_printk" },
	{ 0x83caa58a, "module_layout" },
};
```

`0x83caa58a` on both sides here — this build's module and this exact
kernel agree, so `check_version()` returns `1` for every symbol. This
function is **not** `module_layout`-specific — `check_modstruct_version()`
(4.2) is just one particular caller, hardcoding `"module_layout"` as
`symname`. The exact same function runs again, later, once per *real*
imported symbol during `simplify_symbols()`. Confirmed live: leaving the
`find_symbol` breakpoint armed past the `module_layout` hit and
`continue`-ing once more, still on the same, successful `better_hello.ko`
load:

```gdb
(gdb) continue
Thread 2 hit Breakpoint 2, find_symbol (fsa=fsa@entry=0xffff80008483b698)
    at kernel/module/main.c:392
(gdb) print fsa->name
$8 = 0xffff8000848558bf "_printk"
```

Same machinery, same struct, same comparator — now resolving `_printk`
instead of `module_layout`. `module_layout` gets no special-cased kernel
plumbing; it's checked first only because `check_modstruct_version()`
happens to run before `simplify_symbols()` in `load_module()` (Part 3),
using a symbol whose sole purpose is being a stand-in for the module ABI
itself.

(`check_version()` above is the simplified, no-`CONFIG_MODVERSIONS`-extended
path. This kernel's `.config` also has `CONFIG_EXTENDED_MODVERSIONS`
available — a parallel `name\0crc` table pair, `info->index.vers_ext_crc`/
`vers_ext_name`, checked first if present, same comparison in spirit: a
live kernel CRC against one the module recorded for that name.)

### 4.4 — `check_modinfo(mod, info, flags)` and `get_modinfo()`: how the vermagic *string* is actually fetched

```c
// kernel/module/main.c
static int check_modinfo(struct module *mod, struct load_info *info, int flags)
{
	const char *modmagic = get_modinfo(info, "vermagic");

	if (flags & MODULE_INIT_IGNORE_VERMAGIC)
		modmagic = NULL;

	if (!modmagic) {
		return try_to_force_load(mod, "bad vermagic");
	} else if (!same_magic(modmagic, vermagic, info->index.vers)) {
		pr_err("%s: version magic '%s' should be '%s'\n",
		       info->name, modmagic, vermagic);
		return -ENOEXEC;
	}
	return check_modinfo_livepatch(mod, info);
}
```

`mod` here is used for exactly one thing — the `try_to_force_load(mod,
"bad vermagic")` call, i.e. it's only ever touched on the "no vermagic to
compare" branch. `flags` is checked for exactly one bit,
`MODULE_INIT_IGNORE_VERMAGIC` — set that (a `finit_module(2)` flag,
distinct from `MODULE_INIT_IGNORE_MODVERSIONS` from 4.1) and `modmagic`
is forced to `NULL` **before any string is ever compared at all** — this
is a skip, not a lenient comparison. `info` supplies both the raw bytes
`get_modinfo()` scans and the `info->index.vers` value that becomes
`same_magic()`'s third argument (Part 5).

`get_modinfo(info, "vermagic")` → `get_next_modinfo(info, "vermagic",
NULL)`:

```c
static char *get_next_modinfo(const struct load_info *info, const char *tag, char *prev)
{
	Elf_Shdr *infosec = &info->sechdrs[info->index.info];
	unsigned long size = infosec->sh_size;
	char *modinfo = (char *)info->hdr + infosec->sh_offset;   /* raw file bytes, not final module memory */

	if (prev)
		modinfo = module_next_tag_pair(prev, &size);

	for (char *p = modinfo; p; p = module_next_tag_pair(p, &size))
		if (strncmp(p, tag, strlen(tag)) == 0 && p[strlen(tag)] == '=')
			return p + strlen(tag) + 1;
	return NULL;
}

char *module_next_tag_pair(char *string, unsigned long *secsize)
{
	while (string[0]) { string++; if ((*secsize)-- <= 1) return NULL; }  /* skip this tag=value\0 */
	while (!string[0]) { string++; if ((*secsize)-- <= 1) return NULL; } /* skip zero padding */
	return string;
}
```

There is no lookup table, no hashing — `get_modinfo()` is a **linear scan
over the raw `.modinfo` section bytes**, comparing each `NUL`-separated
`tag=value` entry's prefix against the string `"vermagic"` until one
matches. The base pointer is deliberately `info->hdr + sh_offset` — the
in-kernel copy of the *original uploaded file*, addressed by file offset
— not `sh_addr` (final mapped module memory), because, per the source's
own comment, `sh_addr` isn't even set yet the first few times this
function gets called elsewhere in `load_module()`.

Live, at the `check_modinfo` breakpoint, dumping exactly the bytes this
scan walks (`x/Ns` — GDB's "N NUL-terminated strings starting here",
tailor-made for this exact byte layout):

```gdb
(gdb) x/8s (char*)info->hdr + info->sechdrs[info->index.info].sh_offset
0xffff80008483d100:	"description=A simple hello World Linux Kernel Module"
0xffff80008483d135:	"author=guguali"
0xffff80008483d144:	"license=GPL"
0xffff80008483d150:	"srcversion=B4F7C2CAFBE545E9ADB188E"
0xffff80008483d173:	"depends="
0xffff80008483d17c:	"name=better_hello"
0xffff80008483d18e:	"vermagic=7.2.0-kgdb-debug+ SMP preempt mod_unload modversions aarch64"
0xffff80008483d1d4:	"\"\006"
```

Tag for tag, byte for byte, this is the exact same content and order as
Part 1's static `readelf -p .modinfo better_hello.ko` dump — now read
live, out of guest kernel memory, mid-syscall. (The 8th line is scan
overrun past the real `.modinfo` section's end, `x/8s` doesn't know the
section boundary — harmless, and itself a small confirmation that
`get_next_modinfo()`'s own `secsize` bookkeeping, not any built-in string
terminator, is what actually stops the real scan at the right place.)

`try_to_force_load()`, the one escape hatch in this whole chain,
`kernel/module/main.c`:

```c
int try_to_force_load(struct module *mod, const char *reason)
{
#ifdef CONFIG_MODULE_FORCE_LOAD
	if (!test_taint(TAINT_FORCED_MODULE))
		pr_warn("%s: %s: kernel tainted.\n", mod->name, reason);
	add_taint_module(mod, TAINT_FORCED_MODULE, LOCKDEP_NOW_UNRELIABLE);
	return 0;
#else
	return -ENOEXEC;
#endif
}
```

Worth being precise about when this actually runs: only when `modmagic`
is `NULL` — the module's `.modinfo` genuinely has no `vermagic=` tag at
all, **or** `MODULE_INIT_IGNORE_VERMAGIC` zeroed it deliberately. A
vermagic string that's *present but wrong* never reaches
`try_to_force_load()` at all — `check_modinfo()` takes the `else if
(!same_magic(...))` branch straight to `pr_err()` + `return -ENOEXEC`,
full stop, gated additionally by `CONFIG_MODULE_FORCE_LOAD` being
compiled into the *running* kernel in the first place. "Force-loading a
mismatched module" isn't a comparison override anywhere in this code —
it's userspace asking the kernel to skip the comparison before it happens.

---

## Part 5 — `same_magic()`: the part that actually surprised me

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
express. Part 7 proves this live: two builds a whole kernel apart get
rejected by `check_modstruct_version()`'s CRC check, never even reaching
the vermagic string comparison this section is about.

---

## Part 6 — live, Part 1: a matching build, breakpoint by breakpoint

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

`has_crcs=true`, live confirmation of the Part 5 claim — this build really
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

## Part 7 — live, Part 2: a module built against the *wrong* kernel

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
comparison from Parts 1–6 — the one `modinfo | grep vermagic` seems to
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
`bad_version:` label read in Part 4. `invalid module format` is
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
- Every parameter in the chain is traceable to a concrete source, not a
  black box (Part 4): `info->index.vers`/`info->index.info` are real ELF
  section indices *inside this specific `.ko`*, cross-checked live
  against `readelf -S`'s own numbering; `find_symbol_arg`'s `crc`/`owner`/
  `sym` fields go from all-zero to populated across exactly one function
  call, watched live; the vermagic string itself is fetched by a plain
  linear byte-scan over the module's own raw `.modinfo` section — no
  lookup table, no relocation, dumped live and matching Part 1's static
  `readelf` output tag for tag.
- `check_modstruct_version()` — a CRC check on one synthetic symbol,
  `module_layout` — runs *before* the vermagic check and, for any
  `CONFIG_MODVERSIONS` build, is the check that actually decides whether
  a module built for a different kernel gets rejected (Parts 3, 4, 7).
  It isn't special-cased plumbing: it's `find_symbol()` +
  `check_version()`, the *exact* generic machinery every other imported
  symbol (`_printk`, confirmed live) goes through too — `module_layout`
  is just a hardcoded name and a deliberately empty function whose only
  job is to fingerprint the core module ABI structs via its own CRC.
- `same_magic()` deliberately skips the kernel-release word once CRCs are
  present — proven both by reading the function and by reproducing it
  standalone outside the kernel (Part 5) — so the vermagic string's
  headline value, the release string everyone stares at in `modinfo`
  output, isn't actually load-bearing in a modversions build. What is
  load-bearing: the SMP/preempt/mod_unload/modversions/arch flags after
  it, and, one layer earlier and more precisely, `module_layout`'s CRC.
- The kernel doesn't run one fixed algorithm here — it's a small,
  flag-driven decision (Part 4.1, 4.4, 4.5) between three genuinely
  different outcomes: normal comparison (flag-words only, CRCs present),
  `MODULE_INIT_IGNORE_MODVERSIONS` (forces a full string compare,
  release word included), or `MODULE_INIT_IGNORE_VERMAGIC` (skips the
  string comparison entirely and defers to `try_to_force_load()`, itself
  gated by the *running* kernel's own `CONFIG_MODULE_FORCE_LOAD`).
- All of it verified twice, live, on the real debug kernel: once where
  every check passes end to end (Part 6), once where the very first
  kernel-ABI check catches an honestly-wrong-kernel build before the
  vermagic comparison this whole document is nominally about ever runs
  (Part 7).
