# Deployment, packaging, and upstreaming

Every module in this repo ends at `sudo insmod ./module.ko` — you built
it, you loaded it, on your own machine, with your own kernel. That's the
right scope for learning the APIs. It is not how a kernel module reaches
a machine that isn't yours. This document covers the three real answers
to "now what": DKMS (rebuild automatically across kernel updates), module
signing (why every module's `dmesg` output has already shown you a taint
warning), and the actual upstream contribution pipeline this repo's
`checkpatch.pl` habit has been rehearsing pieces of since module 05.

## The taint warning you've already seen

Load any module from this repo and `dmesg` shows something like:

```
gpioctrl: loading out-of-tree module taints kernel.
gpioctrl: module verification failed: signature and/or required key missing - tainting kernel
```

That's not a warning about *this repo's code* — it's the module signing
facility (`Documentation/admin-guide/module-signing.rst` in
`../linux_mainline`) doing exactly what it's designed to do: mark the
running kernel as having loaded something it can't cryptographically
verify. Two separate, real mechanisms are involved, and understanding
both is most of what "deploying a module properly" means.

## Module signing

From the kernel's own documentation (`Documentation/admin-guide/module-signing.rst`):

> The kernel module signing facility cryptographically signs modules
> during installation and then checks the signature upon loading the
> module. This allows increased kernel security by disallowing the
> loading of unsigned modules or modules signed with an invalid key.

Configuration lives under `CONFIG_MODULE_SIG` (`make menuconfig` →
*Enable Loadable Module Support*):

| Option | Effect |
|---|---|
| `CONFIG_MODULE_SIG_FORCE` | Off (default): unsigned/unverifiable modules load anyway but taint the kernel — exactly what every module in this repo has been doing. On: they're rejected outright. |
| `CONFIG_MODULE_SIG_ALL` | Automatically sign every module during `modules_install`. |
| `CONFIG_MODULE_SIG_KEY` | Which private key/certificate to sign with; defaults to an auto-generated one at `certs/signing_key.pem`. |
| `CONFIG_SYSTEM_TRUSTED_KEYS` | Additional X.509 certs to trust at boot, beyond the auto-generated one. |

Signing algorithms supported: RSA, NIST P-384 ECDSA, and NIST
FIPS-204 ML-DSA, with SHA-2/SHA-3 (256/384/512) as the hash.

**Signing a module by hand**, per the same document:

```bash
scripts/sign-file sha512 kernel-signkey.priv kernel-signkey.x509 module.ko
```

Four arguments, in order: hash algorithm, private key, public key
(certificate), and the `.ko` to sign. This is exactly the mechanism
`Documentation/admin-guide/module-signing.rst`'s "Manually signing
modules" section documents — read it directly in
`../linux_mainline` for the full key-generation and stripping
discussion, which is out of scope here.

The practical upshot: **taint isn't a bug to work around for a personal
project like this repo** — `CONFIG_MODULE_SIG_FORCE=off` (the common
default) is specifically permissive for exactly this use case. It becomes
real the moment you're building for a machine (or a distro) with
`MODULE_SIG_FORCE` on, a Secure Boot-locked-down kernel, or any policy
that treats a tainted kernel as unsupported.

## DKMS — surviving a kernel update

Every module here is built against `/lib/modules/$(uname -r)/build` — one
specific kernel. Update the kernel and the `.ko` is gone; you'd have to
`make` again by hand. DKMS (Dynamic Kernel Module Support) is the
standard mechanism that rebuilds an out-of-tree module automatically
every time a new kernel is installed, driven by a small `dkms.conf` file
alongside the source.

**Layout DKMS expects**, source copied to
`/usr/src/<module-name>-<version>/`:

```
/usr/src/gpioctrl-1.0/
├── dkms.conf
├── Makefile
└── gpioctrl.c
```

**A `dkms.conf` for this repo's own `03_gpio_sim/gpioctrl.c`**, using
the real, documented directive names (`PACKAGE_NAME`, `PACKAGE_VERSION`,
`BUILT_MODULE_NAME[#]`, `DEST_MODULE_LOCATION[#]` — the latter is a
*required* directive per the DKMS manual page, specifying where the
built module gets installed under `/lib/modules/<kernel>/`):

```
PACKAGE_NAME="gpioctrl"
PACKAGE_VERSION="1.0"
BUILT_MODULE_NAME[0]="gpioctrl"
DEST_MODULE_LOCATION[0]="/extra"
AUTOINSTALL="yes"
```

**The command sequence**:

```bash
sudo mkdir -p /usr/src/gpioctrl-1.0
sudo cp 03_gpio_sim/{gpioctrl.c,Makefile} /usr/src/gpioctrl-1.0/
# + the dkms.conf above, in the same directory

sudo dkms add -m gpioctrl -v 1.0
sudo dkms build -m gpioctrl -v 1.0
sudo dkms install -m gpioctrl -v 1.0
```

From here, every future `apt upgrade`/`dnf upgrade` that installs a new
kernel triggers DKMS to rebuild `gpioctrl.ko` against it automatically —
no manual `make` required. This is exactly how most real-world
out-of-tree drivers you've ever installed got onto your machine (NVIDIA
and VirtualBox's guest additions are the two most commonly recognized
examples, though the mechanism is generic to any out-of-tree module).

`man 8 dkms` is the authoritative reference for every directive
`dkms.conf` supports — the four above are the minimum to get a single
module building; multi-module packages, patches applied at build time,
and post-build/post-install scripts are all documented there.

## `MODULE_VERSION()` and how distros actually ship a module

None of this repo's modules set `MODULE_VERSION()` — worth adding once a
module is meant to be *packaged*, since it's what `modinfo` and DKMS
version tracking both key off of:

```c
MODULE_VERSION("1.0");
```

Beyond DKMS, the two real distro-native packaging conventions:

- **Debian/Ubuntu**: either a DKMS-backed `.deb` (the package's postinst
  script calls `dkms add`/`build`/`install`, exactly as above, so the
  *user* never runs those commands by hand) or, for modules that ship
  prebuilt for a specific kernel ABI, a `linux-modules-*` style package
  built against that exact kernel version — the same convention this
  machine's own `linux-modules-7.0.0-30-generic` package (visible via
  `dpkg -l | grep linux-modules`) uses.
- **Fedora/RHEL**: `akmod`/`kmod` packages — `akmod` is RPM's rough DKMS
  equivalent (rebuilds from source against the running kernel via a
  `%post` scriptlet), while a plain `kmod-*` package ships a prebuilt
  `.ko` for one specific kernel build, the same tradeoff as Debian's
  prebuilt path.

Both ecosystems converge on the same two choices this document already
covered: rebuild-from-source-on-install (DKMS/akmod) or
ship-a-prebuilt-binary-per-kernel-version — packaging is mostly about
which of those two your module needs, then wrapping the DKMS (or plain
`make`) commands above in the target distro's package format.

## The upstreaming pipeline — where the `checkpatch.pl` habit leads

Modules 05, 07, 08, and every module from 09 onward wire
`scripts/checkpatch.pl --strict` into their `Makefile`. That's not
incidental style-checking — it's the exact tool real kernel patches are
checked with before anyone reviews them. The rest of the pipeline, once
`checkpatch` is clean:

**1. Find the maintainer(s).** `scripts/get_maintainer.pl` (in
`../linux_mainline`) reads a file or patch and prints who actually owns
that code, pulled from the `MAINTAINERS` file plus recent git history.
Real, verified output against the exact driver `03_gpio_sim` talks to:

```bash
$ cd ../linux_mainline
$ scripts/get_maintainer.pl -f drivers/gpio/gpio-sim.c
Linus Walleij <linusw@kernel.org> (maintainer:GPIO SUBSYSTEM)
Bartosz Golaszewski <brgl@kernel.org> (maintainer:GPIO SUBSYSTEM)
linux-gpio@vger.kernel.org (open list:GPIO SUBSYSTEM)
linux-kernel@vger.kernel.org (open list)
```

That's not a hypothetical — that's the real list a patch touching
`gpio-sim.c` would need to go to, generated by the same tool every real
kernel contributor uses before sending anything.

**2. Format and send the patch as plain text, inline — never an
attachment.** `git format-patch` + `git send-email`, or the same
plain-text pasted into a mail client; HTML mail and attachments are
routinely bounced by kernel mailing lists without being reviewed at all.
`Documentation/process/submitting-patches.rst` (in `../linux_mainline`)
is the authoritative source on exact formatting, commit message
conventions (`Signed-off-by:`, what a good "why, not just what" commit
message looks like — the same convention this repo's own commit history
has been following all along), and the review process itself.

**3. Expect review, iterate, resend.** A real patch is very rarely
accepted on the first send. Version your resends (`[PATCH v2]`, ...) and
fold review feedback in — this is a genuine social/technical process,
not a formality.

**A real, currently-maintained walkthrough of this entire pipeline**,
start to finish, written for exactly this "I've never sent a patch
before" moment:
[kernelnewbies.org/FirstKernelPatch](https://kernelnewbies.org/FirstKernelPatch).
It covers environment setup (git identity matching your email exactly,
`mutt`+`esmtp` or `git send-email`), finding a small real change to
make, formatting, `get_maintainer.pl`, sending inline, and handling
feedback — the same pipeline outlined above, in more depth, kept current
by the community that actually reviews these patches. See
[`kernelnewbies.org/FAQ/WhereDoIBegin`](https://kernelnewbies.org/FAQ/WhereDoIBegin)
for more general advice on picking a first real target.

## Which of these actually matters for you

Most real-world driver work — internal company hardware, a hobbyist
board, anything not destined for mainline — never goes through the
upstreaming pipeline at all and stops at DKMS (or a simple systemd
service that runs `insmod` at boot). Upstreaming matters specifically
once you're touching code that *ships in the kernel already* (fixing a
bug in `gpio-sim.c` itself, say, rather than a driver that talks to it)
or you want your own new driver to become part of mainline permanently.
Both are real, common paths — this document covers both because the
honest answer to "how do I deploy a kernel module" depends entirely on
which one you're actually doing.
