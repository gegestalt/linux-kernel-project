# GDB walkthrough — 13_kernel_memory, hands-on, start to finish

`kernel_memory.c` exposes three kernel allocators — `kmalloc()`,
`vmalloc()`, `kmem_cache_alloc()` — behind one sysfs control slot,
tracking exactly one live allocation at a time. The debugging angle this
module is built for: printing the *same kind of pointer* through each
allocator's different lens (`ksize()`'s notion of "actual size" only
applies to `kmalloc`; `vmalloc`'s pages are virtually, not physically,
contiguous; a `kmem_cache` object always comes back exactly
`CACHE_OBJ_SIZE`) — differences invisible from the call site alone that
become concrete the moment you inspect what each call actually returned.

Every command below says exactly which pane. One command per step,
always — paste it, wait for the prompt to come back, then the next one.

`do_allocate()` — like module 11's `increment_once()` — has no
standalone symbol; it's inlined into `allocate_store()` (confirmed
statically before this walkthrough was written: `info line do_allocate`
reports an address *inside* `allocate_store`). Break on `allocate_store`
and step through; there's no separate frame for `do_allocate` in a
backtrace.

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
cd 13_kernel_memory
make -C /home/adiopocere/Desktop/codes/linux_mainline M=$(pwd) modules
```
```bash
modinfo kernel_memory.ko | grep vermagic
```
```bash
sudo mount -o loop /home/adiopocere/Desktop/codes/qemu-vmb/labs-disk.img /tmp/vmb-mnt
sudo mkdir -p /tmp/vmb-mnt/13_kernel_memory
sudo cp kernel_memory.ko /tmp/vmb-mnt/13_kernel_memory/
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

## Step 3 — start gdb, connect

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

## Step 4 — load the module

**Pane: gdb**

```
break do_init_module
```
```
continue
```

**Pane: vmb**

```bash
insmod /mnt/labs/13_kernel_memory/kernel_memory.ko
```

**Pane: gdb**

```
lx-symbols /home/adiopocere/Desktop/codes/linux-kernel-project
```

**Pane: vmb**

```bash
cat /sys/kernel/kernel_memory/info
```

Should read `type=none requested_bytes=0 actual_bytes=0 ...`.

## Step 5 — `kmalloc`: watch `ksize()` diverge from the request

**Pane: gdb**

```
break allocate_store
```
```
continue
```

**Pane: vmb**

```bash
echo "kmalloc 100" | tee /sys/kernel/kernel_memory/allocate
```

**Pane: gdb**

```
Thread 2 hit Breakpoint 2, allocate_store (...) at kernel_memory.c:207
```
```
next
```
```
print cmd
```
```
$1 = 0x... "kmalloc"
```
```
next
```
```
print size
```
```
$2 = 100
```
```
next
```
```
next
```
```
print ptr
```
```
$3 = (void *) 0xffff...
```

This address is a real, mapped kernel address (freshly `kmalloc()`'d,
uninitialized — don't expect meaningful bytes, just confirm it's
readable and doesn't fault, unlike module 10's raw userspace `argp`).

```
next
```
```
print cur_actual_size
```
```
$4 = 128
```

**128, not 100.** You asked for 100 bytes; `kmalloc()`'s slab allocator
only hands out from a fixed set of bucket sizes, and rounds your request
up to the next one — `ksize()` reports the bucket's real capacity, which
is what you actually got. This is directly why a driver that needs to
know its *actual* usable allocation size calls `ksize()` explicitly
rather than assuming it matches the request.

```
finish
```

**Pane: vmb**

```bash
cat /sys/kernel/kernel_memory/info
```

## Step 6 — free it, then `vmalloc` the same nominal size

**Pane: gdb**

```
break do_free
```
```
continue
```

**Pane: vmb**

```bash
echo 1 | tee /sys/kernel/kernel_memory/free
```

**Pane: gdb**

```
Thread 2 hit Breakpoint 3, do_free () at kernel_memory.c:145
```
```
next
```
```
print type
```
```
$5 = ALLOC_KMALLOC
```
```
next
```
```
finish
```
```
delete
```
```
y
```
```
break allocate_store
```
```
continue
```

**Pane: vmb**

```bash
echo "vmalloc 100" | tee /sys/kernel/kernel_memory/allocate
```

**Pane: gdb**

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
print ptr
```
```
$6 = (void *) 0xffffa...
```

Compare this address against step 5's `kmalloc` pointer — on `arm64`
(and most architectures), `vmalloc()`'s return lives in a visibly
different virtual address range than the slab allocator's, because it's
mapped through its own dedicated page tables rather than the kernel's
direct physical-memory mapping. `cur_actual_size` for this branch is
simply `size` unchanged (100, not rounded) — the source's own
`do_allocate()` has `cur_actual_size = (type == ALLOC_KMALLOC) ?
ksize(ptr) : size;` — `ksize()` is only meaningful for `kmalloc`'s
bucket-based allocations.

## Step 7 — the fixed-size cache

**Pane: gdb**

```
delete
```
```
y
```
```
break allocate_store
```
```
continue
```

**Pane: vmb**

```bash
echo 1 | tee /sys/kernel/kernel_memory/free
```
```bash
echo "cache 999" | tee /sys/kernel/kernel_memory/allocate
```

**Pane: gdb**

```
Thread 2 hit Breakpoint N, allocate_store (...) at kernel_memory.c:207
```
```
next
```
```
print size
```
```
$7 = 999
```
```
next
```
```
next
```
```
print size
```
```
$8 = 128
```

`CACHE_OBJ_SIZE` — your requested `999` was silently ignored. The sysfs
interface accepted `"cache 999"` without error, but the cache allocator
only ever hands out objects of the one fixed size it was created with
(`kmem_cache_create("kernel_memory_demo", CACHE_OBJ_SIZE, ...)` at
init) — the size argument is meaningless for this branch, and watching
it get overwritten mid-function is a more convincing demonstration of
that than reading the `switch` statement.

## Step 8 — `-EBUSY`: only one allocation tracked at a time

**Pane: gdb**

```
continue
```

**Pane: vmb** (don't free first this time)

```bash
echo "kmalloc 50" | tee /sys/kernel/kernel_memory/allocate
```

**Pane: gdb**

```
Thread 2 hit Breakpoint N, allocate_store (...) at kernel_memory.c:207
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
finish
```

**Pane: vmb**

```bash
echo "kmalloc 50" | tee /sys/kernel/kernel_memory/allocate
```
```
tee: write error: Device or resource busy
```

## Step 9 — `stats`: cumulative counters across everything above

**Pane: gdb**

```
delete
```
```
y
```
```
break stats_show
```
```
continue
```

**Pane: vmb**

```bash
cat /sys/kernel/kernel_memory/stats
```

**Pane: gdb**

```
Thread 2 hit Breakpoint N, stats_show (...) at kernel_memory.c:294
```
```
finish
```

**Pane: vmb**

```bash
cat /sys/kernel/kernel_memory/stats
```

`alloc_ok` should read `3` (the kmalloc, vmalloc, and cache allocations
from steps 5–7); whether `alloc_fail` includes step 8's `-EBUSY`
rejection depends on the driver's own accounting — check `do_allocate()`'s
source: the `EBUSY` early-return happens *before* `stats_alloc_fail++`
is ever reached, so it counts toward neither `alloc_ok` nor
`alloc_fail`, a real, slightly surprising fact worth confirming by
stepping through it exactly as step 8 did rather than assuming from the
field name alone.

## Step 10 — the exit path

`kernel_memory_exit` is marked `__exit`, placed in its own `.exit.text`
section, which `lx-symbols` never relocates — `break kernel_memory_exit`
right now would silently resolve to a raw, unrelocated file offset.
Break on the generic unload hook instead:

**Pane: gdb**

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
rmmod kernel_memory
```

**Pane: gdb**

```
advance kernel/module/main.c:863
```
```
print mod->exit
```
```
$9 = (void (*)(void)) 0xffff80007c3207f8
```

(Your address will differ — module memory placement is random per boot
even with `nokaslr`.)

```
add-symbol-file /home/adiopocere/Desktop/codes/linux-kernel-project/13_kernel_memory/kernel_memory.ko -s .exit.text 0xffff80007c3207f8
```
```
y
```
```
break kernel_memory_exit
```
```
Breakpoint N at 0xffff80007c3207f8: file kernel_memory.c, line 353. (2 locations)
```
```
disable N.1
```
```
continue
```

**Pane: vmb**

```bash
rmmod kernel_memory
```

**Pane: gdb**

```
Thread N hit Breakpoint N.2, kernel_memory_exit () at kernel_memory.c:353
353		if (cur_type != ALLOC_NONE) {
```
```
next
```
```
print cur_type
```

If you left an allocation live (step 7's cache object, most likely, if
you followed the steps above in order without an extra manual free),
this branch fires and calls `do_free()` for you — this is the driver's
own safety-net free on unload. If you already freed everything manually,
`cur_type` reads `ALLOC_NONE` and the branch is skipped.

## Step 11 — clean up

**Pane: gdb**

```
continue
```
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

Three allocators that all "just return a pointer" from the call site
alone produce pointers with genuinely different addresses (step 6),
different actual-vs-requested size behavior (`ksize()` meaning something
for `kmalloc` and nothing for the other two — steps 5–7), and different
failure semantics (step 8) — none of which is visible without actually
inspecting what came back. Stepping through all three live is a faster
way to internalize this than reading `include/linux/slab.h`'s comments
alone.
