# 13 — kernel_memory

Three kernel memory allocators, compared directly through one sysfs
interface: `kmalloc()`, `vmalloc()`, and a dedicated `kmem_cache`. Same
allocation slot, same instrumentation, different backing allocator — so
the differences you see are the allocators', not the driver's.

## What this demonstrates

- **`kmalloc()`/`kzalloc()`** — the general-purpose slab allocator.
  Physically contiguous (the only one of the three safe for DMA), backed
  by fixed-size buckets, so `ksize()` on the returned pointer usually
  reports *more* than you asked for (whatever the bucket size rounded up
  to). Has a hard ceiling, `KMALLOC_MAX_SIZE`, printed to `dmesg` at load
  time and exposed via sysfs — because finding a physically contiguous
  run of pages gets harder the larger the request.
- **`vmalloc()`** — maps individually-allocated, possibly scattered
  physical pages into one virtually contiguous range. No DMA guarantee,
  and each allocation costs a page-table/TLB setup that `kmalloc()`
  doesn't pay — but no meaningful size ceiling either. This is the
  allocator lab [09](../09_read_write_cdev/)'s fixed 4KB buffer *didn't*
  need, and this lab's `vmalloc` mode is where you can push well past
  what `kmalloc` will ever hand back.
- **A dedicated `kmem_cache`** (`kmem_cache_create()` at module load,
  `kmem_cache_alloc()`/`kmem_cache_free()` per use) — a slab cache
  pre-configured for one fixed object size. Real subsystems that
  repeatedly allocate/free many same-sized objects (inodes, `task_struct`,
  network buffers) use their own cache instead of the shared `kmalloc`
  buckets, trading flexibility for speed and less internal fragmentation.
- **Per-allocation timing** (`ktime_get_ns()` around the call) so you can
  compare allocator latency directly instead of taking "vmalloc is
  slower" on faith.
- **A bare `kobject`** under `kernel_kobj`
  (`/sys/kernel/kernel_memory/`) instead of a device-backed sysfs group —
  the right tool when there's no `/dev` node or read/write hot path to
  hang attributes off of, just top-level control/inspection files.

## Files

| File | Purpose |
|---|---|
| `kernel_memory.c` | The module: one tracked allocation slot, three allocator backends, sysfs `allocate`/`free`/`info`/`stats`. |
| `Makefile` | Build, `clean`, `check`/`checkpatch`. |

## Build

```bash
cd 13_kernel_memory
make
```

## Load and test

```bash
sudo insmod ./kernel_memory.ko
dmesg | tail -3         # prints this kernel's actual KMALLOC_MAX_SIZE
cat /sys/kernel/kernel_memory/stats
```

Allocate, inspect, free — one allocator at a time:

```bash
echo "kmalloc 100" | sudo tee /sys/kernel/kernel_memory/allocate
cat /sys/kernel/kernel_memory/info
# type=kmalloc
# requested_bytes=100
# actual_bytes=128        <- rounded up to kmalloc's nearest bucket size
# last_alloc_ns=...
# last_free_ns=0
echo 1 | sudo tee /sys/kernel/kernel_memory/free
```

```bash
echo "cache 999" | sudo tee /sys/kernel/kernel_memory/allocate  # size is ignored for cache
cat /sys/kernel/kernel_memory/info
# type=cache
# requested_bytes=128     <- forced to CACHE_OBJ_SIZE regardless of what you asked
echo 1 | sudo tee /sys/kernel/kernel_memory/free
```

Only one allocation is tracked at a time — a second `allocate` without
freeing the first is rejected:

```bash
echo "kmalloc 4096" | sudo tee /sys/kernel/kernel_memory/allocate
echo "vmalloc 4096" | sudo tee /sys/kernel/kernel_memory/allocate
# tee: ...: Device or busy
echo 1 | sudo tee /sys/kernel/kernel_memory/free
```

Find kmalloc's actual ceiling on this kernel and watch it fail exactly
there, then succeed with `vmalloc` at the same size:

```bash
MAX=$(grep -oP 'kmalloc_max_size=\K[0-9]+' /sys/kernel/kernel_memory/stats)
echo "kmalloc size ceiling on this kernel: $MAX bytes"

echo "kmalloc $MAX" | sudo tee /sys/kernel/kernel_memory/allocate   # succeeds, right at the edge
echo 1 | sudo tee /sys/kernel/kernel_memory/free

echo "kmalloc $((MAX * 2))" | sudo tee /sys/kernel/kernel_memory/allocate
# tee: ...: Cannot allocate memory  -- past KMALLOC_MAX_SIZE, always fails
dmesg | tail -2

echo "vmalloc $((MAX * 2))" | sudo tee /sys/kernel/kernel_memory/allocate
# succeeds -- this is exactly the case vmalloc() exists for
cat /sys/kernel/kernel_memory/info
echo 1 | sudo tee /sys/kernel/kernel_memory/free
```

Compare timing across allocators for the same size (bash loop, informal
but revealing — repeat a few times, the relative ordering is what matters
more than any single reading):

```bash
for type in kmalloc vmalloc cache; do
	sz=128
	echo "$type $sz" | sudo tee /sys/kernel/kernel_memory/allocate > /dev/null
	grep -E 'type|last_alloc_ns' /sys/kernel/kernel_memory/info
	echo 1 | sudo tee /sys/kernel/kernel_memory/free > /dev/null
done
```

## checkpatch

```bash
make check
```

## Cleanup

```bash
sudo rmmod kernel_memory
dmesg | tail -5     # final alloc_ok/alloc_fail/free_count
make clean
```

## Things to try

- `rmmod` the module *without* freeing the current allocation first —
  confirm from `dmesg` that `kernel_memory_exit()` notices and frees it
  for you, and think about why a real driver leaking memory on unload
  (instead of defensively cleaning up like this) is a bug that's easy to
  never notice until the module gets loaded and unloaded enough times to
  matter.
- Request an absurd `vmalloc` size (bigger than physical RAM) and watch
  it fail too, just at a much higher ceiling than `kmalloc` — `vmalloc`
  has no *architectural* limit anywhere near `kmalloc`'s, but it's still
  bounded by how much free memory (and free virtual address space) the
  system actually has.
- Read `mm/slab_common.c` and `mm/vmalloc.c` in `../../linux_mainline` for
  what `kmalloc()`/`vmalloc()` actually do under the hood — in particular
  find where `vmalloc()` walks page tables to build the virtual mapping,
  the exact cost this lab's timing numbers are measuring.
- Add a fourth mode using `kzalloc()`/`vzalloc()` (zeroed variants) and
  compare timing against the non-zeroing versions at a large size — the
  zeroing pass is real work, and vmalloc's zeroing in particular touches
  every page it just mapped.

## Debugging with GDB

Setup: [`../GDB_DEBUGGING.md`](../GDB_DEBUGGING.md).

```gdb
(gdb) lx-symbols /home/adiopocere/Desktop/codes/linux-kernel-project
(gdb) break do_allocate
(gdb) continue
```
```bash
echo "kmalloc 100" | sudo tee /sys/kernel/kernel_memory/allocate
```
```gdb
(gdb) next                # step past the switch() into the kmalloc() call
(gdb) finish                # run to return - `ptr` is now a real slab pointer
(gdb) print ptr
(gdb) print cur_actual_size   # already computed via ksize() by C code, not GDB
```

Note that last one: standard KGDB targets generally don't support
calling arbitrary kernel functions from the prompt (`print ksize(ptr)`
directly would be unreliable at best), which is exactly why this
driver's own `cur_actual_size = ksize(ptr)` line matters — you're
reading a value the *kernel* computed, not asking GDB to compute it.
Rerun with `vmalloc` instead of `kmalloc` at the same breakpoint and
compare `finish`'s reported time-to-return between the two — a rougher
but real-time echo of the `last_alloc_ns` this lab's sysfs `info`
attribute already reports.

