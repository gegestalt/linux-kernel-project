# GDB walkthrough — 13_kernel_memory

`kernel_memory.c` exposes three kernel allocators — `kmalloc()`,
`vmalloc()`, `kmem_cache_alloc()` — behind one sysfs control slot,
tracking exactly one live allocation at a time. The debugging angle
this module is built for: printing the *same* pointer through each
allocator's different lens (`ksize()`'s notion of "actual size" only
applies to `kmalloc`; `vmalloc`'s pages are virtually, not physically,
contiguous; a `kmem_cache` object always comes back exactly
`CACHE_OBJ_SIZE`) — differences that are invisible from the call site
alone but become concrete the moment you inspect what each call
actually returned.

## Environment

```bash
cd 13_kernel_memory
make -C /home/adiopocere/Desktop/codes/linux_mainline M=$(pwd) modules
modinfo kernel_memory.ko | grep vermagic
sudo mount -o loop /home/adiopocere/Desktop/codes/qemu-vmb/labs-disk.img /tmp/vmb-mnt
sudo mkdir -p /tmp/vmb-mnt/13_kernel_memory
sudo cp kernel_memory.ko /tmp/vmb-mnt/13_kernel_memory/
sudo umount /tmp/vmb-mnt
```

## tmux layout

Standard `vmb` + `gdb` panes inside the `kgdb` tmux session — see [`../gdb_debugging.md`](../gdb_debugging.md). **One gdb command per paste, always** — a multi-line paste can get merged into one bogus command instead of running one line per Enter (that doc's third gotcha rule).

## Real, verified breakpoint targets

```
Line 207: allocate_store   (do_allocate() is inlined into this - see below)
Line 145: do_free
Line 266: info_show
Line 294: stats_show
Line 324: kernel_memory_init
Line 353: kernel_memory_exit
```

Like module 11's `increment_once()`, `do_allocate()` has no standalone
symbol — confirmed statically:

```
$ gdb -q -batch -nx -ex "file kernel_memory.ko" -ex "info line do_allocate" kernel_memory.ko
Line 91 of "kernel_memory.c" starts at address 0x4d8 <allocate_store+336> ...
```

Inlined into `allocate_store` — `break allocate_store` and step
through; there's no separate frame for `do_allocate` to show up in a
backtrace.

## The walkthrough

### Step 1 — load and confirm nothing is allocated yet

```gdb
(gdb) target remote :1234
(gdb) lx-version
(gdb) break do_init_module
(gdb) continue
```
```bash
# vmb:
insmod /mnt/labs/13_kernel_memory/kernel_memory.ko
```
```gdb
(gdb) lx-symbols /home/adiopocere/Desktop/codes/linux-kernel-project
```
```bash
# vmb:
cat /sys/kernel/kernel_memory/info
```
Should read `type=none requested_bytes=0 actual_bytes=0 ...`.

### Step 2 — `kmalloc`: watch `ksize()` diverge from the request

```gdb
(gdb) break allocate_store
(gdb) continue
```
```bash
# vmb:
echo "kmalloc 100" | tee /sys/kernel/kernel_memory/allocate
```
```gdb
Thread 2 hit Breakpoint N, allocate_store (...) at kernel_memory.c:207
(gdb) next    # past the kbuf copy, strim(), strchr() split
(gdb) print cmd
$1 = 0x... "kmalloc"
(gdb) next     # kstrtoul(strim(size_str), 0, &size)
(gdb) print size
$2 = 100
(gdb) next      # do_allocate(type, size) - inlined, `next` walks straight into its body
(gdb) next       # ptr = kmalloc(size, GFP_KERNEL)
(gdb) print ptr
$3 = (void *) 0xffff...
```

This address is a real, mapped kernel address you could in principle
`x/100xb` right now (it's freshly `kmalloc()`'d, uninitialized —
don't expect meaningful bytes, just confirm it's readable and doesn't
fault, unlike module 10's raw userspace `argp`).

```gdb
(gdb) next    # cur_actual_size = ksize(ptr)  - only for ALLOC_KMALLOC
(gdb) print cur_actual_size
$4 = 128
```

**128, not 100.** You asked for 100 bytes; `kmalloc()`'s slab allocator
only hands out from a fixed set of bucket sizes, and rounds your
request up to the next one — `ksize()` reports the bucket's real
capacity, which is what you actually got, not what you asked for. This
is directly why a driver that needs to know its *actual* usable
allocation size (rather than just the size it requested) calls
`ksize()` explicitly rather than assuming they're the same number.

```gdb
(gdb) finish
```
```bash
# vmb:
cat /sys/kernel/kernel_memory/info
```

### Step 3 — free it, then `vmalloc` the same nominal size

```gdb
(gdb) break do_free
```
Wait — `do_free()` is a real, standalone symbol (unlike `do_allocate`)
— confirm before relying on it:
```
$ gdb -q -batch -nx -ex "file kernel_memory.ko" -ex "info line do_free" kernel_memory.ko
Line 145 of "kernel_memory.c" starts at address 0x1a8 <do_free> ...
```
```gdb
(gdb) continue
```
```bash
# vmb:
echo 1 | tee /sys/kernel/kernel_memory/free
```
```gdb
Thread 2 hit Breakpoint N, do_free () at kernel_memory.c:145
(gdb) next   # type = cur_type; ptr = cur_ptr;
(gdb) print type
$5 = ALLOC_KMALLOC
(gdb) next    # kfree(ptr) via the switch
(gdb) finish
```

```gdb
(gdb) delete <the do_free breakpoint's number — `info breakpoints` if unsure>
(gdb) break allocate_store
(gdb) continue
```

(Bare `delete` with no argument deletes *every* breakpoint, but first
asks `Delete all breakpoints? (y or n)` — if you're typing ahead, that
prompt can silently swallow your next command instead of actually
deleting anything. Naming the number skips the prompt; same reasoning
applies to every `delete` in the rest of this walkthrough.)

```bash
# vmb:
echo "vmalloc 100" | tee /sys/kernel/kernel_memory/allocate
```
```gdb
(gdb) next
(gdb) next
(gdb) next
(gdb) print ptr
$6 = (void *) 0xffffa...    # a very different-looking address range than the kmalloc one
```

Compare this address against step 2's `kmalloc` pointer — on `arm64`
(and most architectures), `vmalloc()`'s return lives in a visibly
different virtual address range than the slab allocator's, because
it's mapped through its own dedicated page tables rather than the
kernel's direct physical-memory mapping. `cur_actual_size` for this
branch is simply `size` unchanged (100, not rounded) — check the
source's own `do_allocate()`: `cur_actual_size = (type ==
ALLOC_KMALLOC) ? ksize(ptr) : size;` — `ksize()` is only meaningful for
`kmalloc`'s bucket-based allocations; `vmalloc`'s size is exactly what
you asked for because it isn't drawing from fixed-size buckets at all.

### Step 4 — the fixed-size cache

```gdb
(gdb) delete <the allocate_store breakpoint's number>
(gdb) break allocate_store
(gdb) continue
```
```bash
# vmb:
echo 1 | tee /sys/kernel/kernel_memory/free
echo "cache 999" | tee /sys/kernel/kernel_memory/allocate
```
```gdb
Thread 2 hit Breakpoint N, allocate_store (...) at kernel_memory.c:207
(gdb) next
(gdb) print size
$7 = 999
(gdb) next
```

Step into the inlined `do_allocate()`'s own body and watch the size get
overridden before the allocation call even happens:

```gdb
(gdb) next    # `if (type == ALLOC_CACHE) size = CACHE_OBJ_SIZE;`
(gdb) print size
$8 = 128    # CACHE_OBJ_SIZE - your requested 999 was silently ignored
```

The sysfs interface accepted `"cache 999"` without error, but the
cache allocator only ever hands out objects of the one fixed size it
was created with (`kmem_cache_create("kernel_memory_demo",
CACHE_OBJ_SIZE, ...)` at init) — the size argument is meaningless for
this branch, and watching it get overwritten mid-function is a more
convincing demonstration of that than reading the `switch` statement.

### Step 5 — `-EBUSY`: only one allocation tracked at a time

```gdb
(gdb) continue
```
Don't `free` first this time:
```bash
# vmb:
echo "kmalloc 50" | tee /sys/kernel/kernel_memory/allocate
```
```gdb
Thread 2 hit Breakpoint N, allocate_store (...) at kernel_memory.c:207
(gdb) next
(gdb) next
(gdb) next    # into do_allocate - the `if (cur_type != ALLOC_NONE)` guard fires immediately
(gdb) finish
```
```bash
# vmb:
echo "kmalloc 50" | tee /sys/kernel/kernel_memory/allocate
# tee: write error: Device or resource busy
```

### Step 6 — `stats`: cumulative counters across everything above

```gdb
(gdb) delete <the allocate_store breakpoint's number>
(gdb) break stats_show
(gdb) continue
```
```bash
# vmb:
cat /sys/kernel/kernel_memory/stats
```
```gdb
Thread 2 hit Breakpoint N, stats_show (...) at kernel_memory.c:294
(gdb) finish
```
```bash
# vmb:
cat /sys/kernel/kernel_memory/stats
```
`alloc_ok` should read 3 (the kmalloc, vmalloc, and cache allocations
from steps 2–4), `alloc_fail` should include the `-EBUSY` rejection
from step 5 — **or not**, depending on whether you consider `-EBUSY`
an allocation "failure" at all; check `do_allocate()`'s own source
again: the `EBUSY` early-return happens *before* `stats_alloc_fail++`
is ever reached, so it counts toward neither `alloc_ok` nor
`alloc_fail` — a real, slightly surprising fact about this driver's
own accounting, worth confirming by stepping through it exactly as
step 5 did rather than assuming from the field name alone.

## Cleanup

**`break kernel_memory_exit` does not work if you try it directly —
confirmed live.** `kernel_memory_exit` is marked `__exit`, placing it
in its own ELF section, `.exit.text`, which `lx-symbols` never
relocates (its hardcoded section list in `scripts/gdb/linux/symbols.py`
doesn't include `.init.text`/`.exit.text`). The breakpoint silently
resolves to a raw, unrelocated file offset instead of a real address —
no error, it just never fires. This affects every module in this repo
using the modern `module_exit()` macro (every module except 01).

**The fix, verified live** — break on the generic kernel hook that
calls into every module's exit function, then read the real address
out of the kernel's own struct:

```gdb
(gdb) delete <the stats_show breakpoint's number>
(gdb) break __do_sys_delete_module
(gdb) continue
```
```bash
# vmb:
rmmod kernel_memory
```
```gdb
(gdb) advance kernel/module/main.c:863
(gdb) print mod->exit
$N = (void (*)(void)) 0xffff80007c3207f8
```

(That address is from one real run and won't match yours — module
memory placement is random per boot regardless of `nokaslr`. Always use
whatever `print mod->exit` gives you right now.) **Do not `step` into
it from here** — with no relocated line table GDB can't bound the
function and `step` free-runs straight past it; `Ctrl-C` recovers you.
Register the section the way `lx-symbols` does for the sections it
already knows about, and the normal breakpoint then resolves cleanly:

```gdb
(gdb) add-symbol-file /home/adiopocere/Desktop/codes/linux-kernel-project/13_kernel_memory/kernel_memory.ko -s .exit.text 0xffff80007c3207f8
(gdb) break kernel_memory_exit
Breakpoint N at 0xffff80007c3207f8: file kernel_memory.c, line 353.
(gdb) delete <the __do_sys_delete_module breakpoint's number>
(gdb) continue
```
```bash
# vmb:
rmmod kernel_memory
```
```gdb
Thread N hit Breakpoint N, kernel_memory_exit () at kernel_memory.c:353
353		if (cur_type != ALLOC_NONE) {
(gdb) next   # the safety-net free on unload
```

If you left an allocation live (step 4's cache object, most likely, if
you followed the steps above in order without an extra manual free),
this branch fires and calls `do_free()` for you — confirm directly:

```gdb
(gdb) print cur_type
```

If this reads anything other than `ALLOC_NONE`, you're about to watch
the defensive cleanup path run; if you already freed everything
manually, this branch is skipped and `cur_type` reads `ALLOC_NONE`
already.

```gdb
(gdb) continue
```
```bash
# vmb:
poweroff -f
```

## What this proves

Three allocators that all "just return a pointer" from the call site
alone produce pointers with genuinely different addresses, different
actual-vs-requested size behavior, and different failure semantics —
none of which is visible without actually inspecting what came back.
`ksize()` only meaning something for `kmalloc`, and a fixed-size cache
silently discarding a requested size that doesn't match its own
configuration, are the kind of detail a driver author has to know
cold; stepping through both live is a faster way to internalize them
than reading `include/linux/slab.h`'s comments alone.
