# Universal Application Compatibility

**Status:** 📐 designed, implementation begins at v0.6
**ADRs:** [ADR-012](#adr-012), [ADR-013](#adr-013), [ADR-014](#adr-014)

AfriyieOS aims to run software built for other systems: Linux packages from every
distribution, Windows `.exe` and `.dll`, Android `.apk` and `.aab`, macOS `.dmg`,
and the containerised/portable formats (AppImage, Flatpak, Snap, MSI, RPM, deb,
pacman). This document is how that is going to work, what it costs, and — plainly
— what will not work.

It is written before the code because the alternative is discovering the
architecture by accident, one compatibility bug at a time.

---

## 1. The honest framing

"Run everything" is not one problem. It is four problems of very different
difficulty, and pretending otherwise produces a plan that fails late.

| Tier | What | Difficulty | Honest verdict |
| --- | --- | --- | --- |
| **1. Native** | AfriyieOS binaries | Trivial | Already works |
| **2. Source-built** | Linux packages whose sources build against our ABI | Moderate | **The real answer.** Most of "all Linux apps" |
| **3. ABI translation** | Linux ELF, Windows PE, Android APK | Hard | Works for a large, well-understood subset. Wine and WSL1 prove it. Nobody has done all of it |
| **4. Proprietary framework emulation** | macOS `.dmg`, some Windows apps, DRM'd Android apps | Very hard to impossible | **Partial at best.** Say so now |

### What genuinely will not work, and why

- **macOS applications.** A `.dmg` is a disk image; the app inside is Mach-O and
  links against Cocoa, AppKit, CoreFoundation and dozens of proprietary
  frameworks. Those are not documented, not redistributable, and their behaviour
  is not specified anywhere we can implement against. We can *read* a DMG and
  *parse* Mach-O. We cannot run a real AppKit app. Anyone claiming otherwise is
  describing a virtual machine running macOS, which is a licensing question, not
  an engineering one.
- **Kernel-mode drivers.** Windows `.sys`, Linux `.ko`, macOS kexts. These are
  written against another kernel's internals. There is no translation layer that
  makes a Linux PCI driver run on a microkernel; the interfaces are the
  subsystem. Userspace drivers recompiled from source are a different matter.
- **Anti-cheat, DRM, and kernel-adjacent copy protection.** These detect
  emulation by design and are built to defeat it. Not a technical gap we can
  close by trying harder.
- **`.aab` is not an app.** An Android App Bundle is a *publishing* format — a
  bundle of split APKs plus Play-specific metadata. It is not installable on a
  device. The correct handling is to run `bundletool`-equivalent logic to
  generate a device-specific APK set, then install that. Shipping "AAB support"
  as if it were "APK support" would be a category error.

Naming these is not pessimism. A compatibility layer that quietly fails on 40% of
apps is worse than one that states its boundary, because the boundary is what
users plan around.

---

## 2. Why a microkernel is the right shape for this

Every mainstream OS that runs foreign binaries does it by *pretending*, and the
pretence always leaks into the kernel:

- Wine's `wineserver` needs `ptrace`, `/proc`, signals and shared memory.
- WSL1 is a Linux syscall translator welded into the NT kernel.
- `binfmt_misc` is a kernel hook that runs an interpreter for unrecognised files.

Each of those is a kernel that grew a second personality. AfriyieOS has a
structural advantage: **the microkernel has no user-visible ABI beyond
capabilities and IPC, and everything else is a user-space server.** A personality
layer is therefore not a modification to the kernel. It is a server that:

1. receives an `exec` request with a capability to a file,
2. identifies the format,
3. builds an address space,
4. installs a syscall translator,
5. enters the program.

The kernel learns nothing. It gains no Linux-shaped code, no PE-shaped code, and
no `binfmt_misc`. That is the whole argument for this architecture and the reason
the objective is achievable at all.

**Capabilities make it safe.** A foreign program cannot be handed ambient
authority even if it is malicious or broken, because there is no ambient
authority to hand it. A Wine-style app gets exactly the file, window and network
capabilities the user granted, and a Windows API call that tries to do something
outside them returns an error to the app rather than reaching the kernel.

---

## 3. Architecture

```
                     ┌──────────────────────────────────────────┐
   user requests ───► │  exec server  (user space, v0.7)         │
   "run this file"    │                                          │
                     │  1. binfmt_detect()   what IS this?       │
                     │  2. personality_for() which server?       │
                     │  3. spawn that personality                │
                     └───────────────┬──────────────────────────┘
                                     │ IPC
        ┌────────────┬───────────────┼───────────────┬────────────┐
        ▼            ▼               ▼               ▼            ▼
   ┌─────────┐  ┌──────────┐   ┌───────────┐   ┌──────────┐  ┌─────────┐
   │ native  │  │ linux    │   │ windows   │   │ android  │  │ package │
   │ ELF     │  │ ELF +    │   │ PE +      │   │ ART +    │  │ manager │
   │ (v0.6)  │  │ syscalls │   │ Win32 API │   │ framework│  │ (v0.9)  │
   │         │  │ (v0.8)   │   │ (v1.3)    │   │ (v1.4)   │  │         │
   └────┬────┘  └────┬─────┘   └─────┬─────┘   └────┬─────┘  └────┬────┘
        │            │               │              │             │
        └────────────┴───────────────┴──────────────┴─────────────┘
                                     │
                        ┌────────────▼────────────┐
                        │  microkernel            │
                        │  capabilities + IPC     │
                        │  NO foreign ABI. Ever.  │
                        └─────────────────────────┘
```

Three pieces are shared by every personality and must be built once, properly:

### 3.1 Format identification (`binfmt`)

Pure function of a byte range. No kernel involvement, no side effects, fully
unit-testable on the host. It answers one question: *what format is this, and
which personality does it need?*

Detecting the format is genuinely harder than the magic numbers suggest: a
Windows `.exe` and a `.dll` share `MZ`; a `.NET` assembly is a PE with a CLI
header; an AppImage is an ELF with a squashfs image appended; a `.deb` may
contain an `ar` archive whose members are further compressed; an APK is a ZIP
whose `classes.dex` may or may not exist (a resource-only split does not have
one). The detector walks inward until it can name something actionable.

### 3.2 The syscall translation layer

Each personality installs a translator that maps the foreign ABI onto our
capability and IPC primitives:

| Foreign | Becomes |
| --- | --- |
| `open()`/`CreateFile` | capability lookup + IPC to the file server |
| `read`/`write` | IPC to the file or device server |
| `socket` | capability to the network server |
| `mmap` | `sys_mem_alloc` + `sys_mem_map` |
| `fork`/`CreateProcess` | `sys_process_spawn` |
| `ioctl` | **the hard one** — see below |

`ioctl` is where compatibility layers go to die. It is a catch-all that exposes
device internals directly. The policy is: implement the ones with documented,
stable semantics (terminals, `FIONREAD`, a small DRM-less set), and for the rest
return `ENOTTY` **and log it**, so the gap is visible and countable rather than
mysterious. Every unsupported ioctl that gets logged is a candidate for the next
piece of work.

### 3.3 The package manager

> **Measured at v0.6, and it changed the plan.** A real `dpkg-deb` package,
> built on this machine and fed to the new reader, says two things the
> documentation does not: modern Debian and Ubuntu packages are **Zstandard**
> (`control.tar.zst`, `data.tar.zst` by default), and the gzip ones that older
> packages use need **full DEFLATE** — Huffman-coded blocks, not just stored
> ones. Both are now their own milestone rather than an assumption inside this
> one. Evidence: `docs/releases/evidence/v0.9.0-real-deb-boundary.txt`.


One tool, many back ends. A package is a *description of files and dependencies*,
and every ecosystem's format is a container for that description:

```
    apk / deb / rpm / pacman / AppImage / Flatpak / Snap
                          │
                    read metadata
                          │
                          ▼
    ┌───────────────────────────────────────────────┐
    │  af_package_t                                 │
    │    name, version, arch, dependencies          │
    │    files[]: path, mode, owner, contents       │
    │    scripts: pre-install, post-install         │
    │    provides[], conflicts[]                    │
    └───────────────────┬───────────────────────────┘
                        │
              install into the AFS store
                        │
        ┌───────────────┴───────────────┐
        ▼                               ▼
   native payload                 foreign payload
   (link into the                 (register with the
    AFS versioned store)           matching personality)
```

The insight that makes "all distros" tractable: **we do not need to run `apt`,
`dnf`, `pacman` and `apk`.** We need to *read* their packages. A `.deb` is an
`ar` archive with a `control.tar.*` and a `data.tar.*`; that is a file format,
not a religion. Dependency resolution across ecosystems is then one solver over
one graph, which is also how the same library ends up installed once instead of
five times.

#### 3.3.1 Where the readers actually stand

| Ecosystem | Container | Reader | Verified against |
| --- | --- | --- | --- |
| Alpine `.apk` | gzip chain of tars | **working** | 5 real packages from `dl-cdn.alpinelinux.org` |
| Arch `.pkg.tar.zst` | zstd tar | **working** | synthetic packages, byte-exact |
| Debian/Ubuntu `.deb` | `ar` + `control.tar.gz` + `data.tar.gz` | **working** | a real `dpkg-deb` package |
| Debian/Ubuntu `.deb` (current) | `ar` + `control.tar.zst` + `data.tar.zst` | **working** | synthetic, byte-exact, both members |
| Debian/Ubuntu `.deb` (xz) | `ar` + `control.tar.xz` + `data.tar.xz` | **container done, LZMA missing** | refused by name, never misread |
| Fedora/openSUSE `.rpm` | binary header + archive | not written | — |
| anything in bzip2 | — | refused by name | — |

The remaining gaps are different in kind and worth saying so plainly.

**The `.deb` story is closed for compression and open for codecs.** A `.deb`'s
control and data members are now decompressed according to their **magic bytes**
rather than their file extensions, so gzip and Zstandard both work and the reader
no longer cares what the name claims (ADR-013). That matters because `dpkg-deb`
changed its default to Zstandard underneath the documentation, and a reader that
only knew `.gz` read the wrong member or none at all. What is still missing is
LZMA: an xz member is *detected* and *refused by name*, never misread, which is
the honest half of the work.

**xz is the one that matters, and it is the one with no specification.**
`docs/architecture/xz-lzma2.md` records why: the `.xz` container is specified in
prose, but the LZMA2 chunk format and LZMA itself are defined by the reference
implementation and nothing else. The container layer is written, verified and in
the gate; the codec is a milestone of its own, and it is the weakest position
this project has been in — there is no second description of the format to catch
a misreading, so byte-exact vectors are not a good practice, they are the only
verification that exists.

**The `.rpm` reader is ordinary work** — a binary tag directory instead of a tar
header, with nothing conceptually new — and it is the last reader the
compatibility table needs.

The zstd decoder itself is done. It decodes 48 frames produced by the reference
implementation — six payload shapes at seven compression levels, plus the same
six carrying content checksums — all byte for byte, and it verifies XXH64
checksums when a frame has one. Nine separate defects stood between "decodes raw
blocks" and "decodes what a package manager writes"; they are all in the debug
log, and six of them produced output of the right length with the wrong bytes.

---

## 4. Milestones

Distributed across the existing roadmap rather than replacing it. Each is
independently useful and independently verifiable.

| Version | Deliverable | Verification |
| --- | --- | --- |
| **v0.6** | `binfmt` detection + native ELF personality split from the kernel | Host tests on synthetic headers; boot markers |
| **v0.7** | exec server as a user-space service; personalities become IPC servers | A foreign-format file produces a clear "no personality" error, not a panic |
| **v0.8** | Linux syscall translator: static musl binaries run | A real static `busybox` runs and its output is asserted |
| **v0.9** | Package manager core: deb, rpm, pacman, apk readers | Host tests parse real packages from each ecosystem |
| **v1.0** | Installer + AFS versioned store; AppImage | Install a real package, reboot, run it |
| **v1.3** | PE loader + Win32 API subset | A real Win32 console program runs |
| **v1.4** | Android: DEX/ART + APK install | An APK with a native activity runs |
| **v2.x** | Flatpak, Snap, MSI, container formats | — |

The ordering is deliberate: **Linux first**, because the Linux ABI is documented,
its syscall set is finite and stable, `musl` and `busybox` give a small
dependency-free target to aim at, and it unlocks the largest body of software for
the least work. Windows second, because PE is simple but Win32 is not. Android
third, because ART is a large runtime and the framework is larger. macOS not at
all, for the reasons in §1.

---

## 5. What "efficiently" means here

The objective says "efficiently". Concretely, that is:

- **No interpreter tax for native code.** x86_64 native, ARM64 native. No JIT for
  the common case.
- **One copy of a library.** The AFS versioned store means five apps needing
  `libz` have one `libz`. This is where Linux distributions failed for thirty
  years and it is not optional here.
- **Processes, not virtual machines.** A personality is a process. Starting an
  APK must not cost a guest kernel boot.
- **Startup measured, not claimed.** `sys_process_spawn` to first output, with a
  budget in blueprint §14 like everything else.

---

<a id="adr-012"></a>
## ADR-012: Foreign ABIs are user-space personalities, never kernel code

**Status:** accepted, v0.5
**Context:** running Linux, Windows and Android binaries requires implementing
their system call interfaces. The obvious implementation puts that machinery in
the kernel, as WSL1 and `binfmt_misc` do.
**Decision:** the kernel implements exactly one ABI — capabilities, IPC, memory
and threads. Every foreign ABI is a user-space server that translates to those
primitives. The kernel contains no `#ifdef` for another system and no
Linux-shaped, Windows-shaped or Android-shaped code, ever.
**Consequences:**
- A crash in the Linux personality kills a process, not the system.
- Compatibility work cannot regress kernel correctness, because it cannot touch
  the kernel.
- Foreign programs get capabilities, not ambient authority, so a Windows API call
  reaching outside its grant returns an error to the app.
- Cost: IPC on the hot path. This is the accepted trade and it is measurable —
  ADR-014 sets the budget and the escape hatch.
**Alternatives rejected:** in-kernel translation (WSL1) — makes every foreign bug
a kernel bug; virtual machines — a whole guest kernel per app, which fails the
"efficiently" requirement outright.

---

<a id="adr-013"></a>
## ADR-013: Detect formats by walking inward to an actionable answer

**Status:** accepted, v0.5
**Context:** a file's extension is a hint, not a fact. `.exe` can be a PE, a .NET
assembly or a self-extracting archive. `.apk` is a ZIP. An AppImage is an ELF
with a filesystem glued to it. Deciding "which personality?" by extension will be
wrong often enough to be useless.
**Decision:** `binfmt_detect` is a pure function over a byte range returning a
format and the personality required. It unwraps containers until it reaches
something executable, with a depth limit, and reports *why* it gave up when it
cannot decide.
**Consequences:**
- Fully testable on the host with synthetic headers — no QEMU, no disk image.
- A file with no personality produces a clear diagnostic naming the format found
  and the personality missing, rather than a generic failure.
- The depth limit is required: a malicious file can nest archives to exhaust a
  naive unwrapper.
**Alternatives rejected:** extension matching — wrong too often, and wrong
silently; kernel `binfmt_misc` — puts the decision in the kernel and inverts
ADR-012.

---

<a id="adr-014"></a>
## ADR-014: The IPC cost of a personality is measured and budgeted, not assumed

**Status:** accepted, v0.5
**Context:** ADR-012 accepts IPC on the path of every foreign syscall. That is
only a good trade if the cost is bounded and known.
**Decision:** a foreign syscall through a personality costs a fixed budget,
enforced like every other budget: **< 2× the cost of a native syscall + one IPC
round trip**, measured in CI. If a personality exceeds it, that is a bug in the
personality, and the fix is batching or a shared-memory fast path — *not*
pushing the personality into the kernel.
**Consequences:**
- Compatibility work cannot silently degrade the system's responsiveness.
- The number is a gate, so "it felt fine" is not an argument.
- Batching is the intended optimisation, and it is measurable: `read()` on a pipe
  should not be one IPC per byte.
**Alternatives rejected:** no budget — the objective explicitly asks for
efficiency, and an unbudgeted translation layer is how WSL1 became slow enough to
require WSL2.

---

## 6. References

- [memory-model.md](memory-model.md) — address spaces, which a personality builds
- [ipc-protocol.md](ipc-protocol.md) — the transport every personality uses
- [capability-model.md](capability-model.md) — why a foreign app cannot exceed its grant
- [afs-filesystem.md](../abi/afs-filesystem.md) — the versioned store that makes one library serve five apps
- `kernel/include/afriyie/binfmt.h` — the format detector
