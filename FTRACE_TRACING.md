# Tracing modules live: the organic, in-the-wild workflow

GDB/KGDB (`GDB_DEBUGGING.md`) freezes execution so you can inspect state
line by line. This document is different: it's for finding out **what a
module actually does, live, on the kernel you're already running**,
using the same tools working engineers reach for day to day —
`bpftrace` and `perf`. No reboot, no custom kernel build, and critically:
**no pre-knowledge of the source required.** Every example here starts
from *discovering* what's traceable, not being told a function name in
advance.

## Why bpftrace, not hand-editing `/sys/kernel/debug/tracing/*`

Raw ftrace (writing directly to `set_graph_function`, `current_tracer`,
etc.) is the mechanism underneath, and worth understanding once — but
it's fragile in exactly the way real debugging sessions punish you for:
write an unresolvable symbol and the filter silently fails while tracing
stays on, unfiltered, for the entire system. That happened repeatedly
while building this document, including one run that generated a
190,000-line trace of unrelated syscalls before the mistake was caught.
`bpftrace` doesn't have that failure mode: it validates every probe at
attach time and refuses to start if one doesn't resolve, and its `-l`
flag turns "what can I even trace here" into a first-class question
instead of something you have to already know.

## Step 1: discover what's there — don't assume

You don't need to have read a module's source to find out what it
exposes. List it:

```bash
sudo bpftrace -l 'kprobe:MODNAME_*'
```

Real output, run against `gpioctrl.ko` with no prior knowledge assumed:

```
kprobe:gpioctrl_open
kprobe:gpioctrl_read
kprobe:gpioctrl_release
kprobe:gpioctrl_sample_once
kprobe:gpioctrl_work_fn
kprobe:gpioctrl_write
```

That's the entire menu of what you can hook into, straight from the
kernel's own symbol table — six functions, and now you know all six
without opening a single `.c` file.

## Step 2: a live one-liner

```bash
sudo bpftrace -e '
kprobe:gpioctrl_sample_once { printf("-> %s called by %s[%d]\n", probe, comm, pid); }
kprobe:gpio_sim_get         { printf("     descends into %s\n", probe); }
kprobe:gpio_sim_set         { printf("     descends into %s\n", probe); }
'
```

Real captured output, triggered by a real button press
(`echo pull-up > .../sim_gpio20/pull`) while the probe was running:

```
Attached 3 probes
-> kprobe:gpioctrl_sample_once called by kworker/3:0[88823]
     descends into kprobe:gpio_sim_get
-> kprobe:gpioctrl_sample_once called by kworker/3:0[88823]
     descends into kprobe:gpio_sim_get
     descends into kprobe:gpio_sim_set
-> kprobe:gpioctrl_sample_once called by kworker/3:0[88823]
     descends into kprobe:gpio_sim_get
```

Every line here is live, varying, real behavior — most poll cycles only
touch `gpio_sim_get` (nothing changed), one touches `gpio_sim_set` too
(the button state actually flipped, right when the trigger fired). This
isn't a recording being replayed; it's the module's actual periodic
polling loop, caught in the act, from a completely different module's
own kprobe-instrumented function.

## A real gotcha: names collide

Trying this against `debugfs_sysfs.ko`'s `enabled_store` function
initially just failed outright:

```
ERROR: Unable to attach probe: kprobe:enabled_store.
```

`enabled_store` is an extremely common name for a `kobj_attribute`
toggle handler — checking the *entire* kernel's symbol table found five
**other**, unrelated functions sharing that exact name:

```bash
$ sudo grep -w enabled_store /proc/kallsyms
ffff80008043f7c8 t enabled_store
ffff80008070ca10 t enabled_store
ffff800080825da8 t enabled_store
ffff800081053aa8 t enabled_store
ffff8000818d9150 t enabled_store
```

Bare function names are **not unique** across the kernel — a real
problem, not a contrived one, and exactly the kind of thing "in the
wild" debugging actually runs into. The fix is the professional one:
scope the probe to a specific module.

```bash
sudo bpftrace -l 'kprobe:debugfs_sysfs:*'      # discovery, disambiguated
sudo bpftrace -e 'kprobe:debugfs_sysfs:enabled_store { printf("hit\n"); }'
```

`kprobe:MODULE:FUNCTION` is the general form whenever a bare name is
ambiguous (which, for common words like `open`/`read`/`show`/`store`,
is often).

## Argument access

bpftrace can read a probed function's actual arguments, not just note
that it was called. Real capture, `rw_write(struct file *file, const
char __user *buf, size_t count, loff_t *ppos)` — `count` is the third
argument, `arg2` (zero-indexed):

```bash
sudo bpftrace -e 'kprobe:rw_write { printf("count=%d by %s\n", arg2, comm); }'
```

```
-> rw_write called by tee[138566], count=5
```

`echo -n "hello" | tee /dev/read_write_cdev0` really did write 5 bytes —
confirmed from inside the kernel call itself, not inferred from the
shell command.

## `perf probe` — the alternative discovery/attach path

`perf` is the other standard tool for this, when it has debuginfo to
work with:

```bash
sudo perf probe -m <module> --funcs      # list, same idea as bpftrace -l
sudo perf probe -m <module> <function>   # add a probe
sudo perf record -e probe:<function> -ag -- sleep 5
sudo perf script                          # inspect what fired
```

Whether `perf probe`'s symbol loader works depends on how the module was
built and whether `perf` can locate matching debuginfo for it — treat
`bpftrace -l`/`bpftrace -e` as the more reliable default, and reach for
`perf` when you specifically want its call-graph/flamegraph tooling on
top.

## What's underneath, if you want to know

Both tools ultimately ride on the same kernel kprobe/ftrace
infrastructure. The raw mechanism — writing function names directly to
`/sys/kernel/debug/tracing/set_graph_function` and reading
`/sys/kernel/debug/tracing/trace` — is worth understanding once, mainly
so a `bpftrace`/`perf` failure doesn't feel like a black box. It's also
the one place a full, indented **call graph** (not just "this function
was hit") is easy to get for free via `function_graph`:

```bash
TRACE=/sys/kernel/debug/tracing
echo function_graph | sudo tee $TRACE/current_tracer
echo your_function | sudo tee $TRACE/set_graph_function   # ALWAYS verify this write succeeded
sudo cat $TRACE/set_graph_function | grep -qF your_function || echo "did not apply - do not enable tracing_on"
echo 1 | sudo tee $TRACE/tracing_on
# ... trigger ...
echo 0 | sudo tee $TRACE/tracing_on
sudo cat $TRACE/trace
```

Two things bite you here that `bpftrace` protects you from automatically:
an unresolvable symbol write fails *silently* (verify it, every time, or
risk tracing the whole system unfiltered), and a `static` function small
enough to get inlined has no symbol to filter on at all — trace its
caller instead (this happened for real: `increment_once` inlined into
`race_write`, `do_allocate` inlined into `allocate_store`, both in this
repo, found by the filter write failing and `gdb -batch -ex "info line
func"` confirming the address landed inside a different function).

Real capture from lab 07 (`do_init_module`, depth bounded to 4, filtered
to the interesting lines): a single kprobe on `do_init_module` only
tells you it was *entered*; `function_graph` shows everything called
inside it, letting you actually count how many `_printk()` calls
`printk_log_levels_init()` makes:

```
 2)  printk_log_levels_init [printk_log_levels]() {
 2)    _printk();   (x10)
```

**10**, not the 11 you'd expect from reading the source (`pr_info()`
before and after, plus 9 priority calls in between) — empirical
confirmation that `pr_debug()` really did compile down to nothing on
this kernel, not just "can depend on config" in the abstract.

## Discover, don't assume — for any lab in this repo

```bash
sudo insmod ./your_module.ko
sudo bpftrace -l 'kprobe:*' | grep your_module_prefix
# or, once you know the .ko filename:
nm -D your_module.ko 2>/dev/null; nm your_module.ko | grep ' T \| t '
```

That's the actual starting point for approaching any of these modules
cold — including ones you didn't write yourself.
