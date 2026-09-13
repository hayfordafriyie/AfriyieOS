# Changelog

All notable changes to AfriyieOS are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Pre-1.0, `MINOR` versions track the roadmap milestones defined in
[docs/AfriyieOS-Blueprint.md](docs/AfriyieOS-Blueprint.md).

---

## [Unreleased] — v0.1.0 *Seed*

> **Milestone status:** 🔨 code complete, awaiting a green CI run.
> Full detail: [docs/releases/v0.1.0.md](docs/releases/v0.1.0.md)

### Added

#### Boot (`v0.1`)

- **UEFI boot bridge** (`boot/uefi/`) loaded by firmware from
  `EFI/BOOT/BOOTX64.EFI`
  - complete UEFI 2.10 type definitions, every structure in specification order,
    packed, and guarded by static asserts on both total size and the offset of
    every member that is called
  - `EFI_BOOT_SERVICES` lists all 45 members including the unused ones, because
    omitting any of them shifts every later offset
  - entry point declared `__attribute__((ms_abi))` — UEFI uses the Microsoft x64
    ABI regardless of the host compiler
  - watchdog disabled; GOP framebuffer recorded including the stride
  - memory reserved at `0x100000` with `AllocateAddress` so firmware fails loudly
    rather than relocating the kernel
  - UEFI memory map normalised into `af_memory_region_t`; framebuffer regions
    overridden so the PMM can never allocate video memory
  - `ExitBootServices` retry loop with a refetched `MapKey`
  - kernel embedded with `incbin`, removing the file-system code path and
    structurally guaranteeing bridge and kernel come from the same commit

- **Kernel entry** (`kernel/arch/x86_64/entry.asm`) — own stack, `.bss` cleared,
  `kmain` called

- **CPU initialisation** — 64-bit GDT with final selector numbering, 256-entry
  IDT with 48 real NASM stubs, PICs remapped from `0x08` to `0x20`

#### Display (`v0.1`)

- **Framebuffer** — surface init with format and pitch validation; packed-RGB to
  native pixel conversion for `RGBX8888`, `BGRX8888` and `RGB565`
- **Primitives** — clipped pixel, rectangle, outline, lines, vertical gradient,
  alpha blending, rounded rectangles, drop shadow
- **Text** — complete 8×16 printable-ASCII bitmap font, scalable, measurable
- **Splash screen** — responsive: a centred card in landscape, a stacked layout in
  portrait. The first version of the breakpoint rule the Shell adopts at v0.8

#### Diagnostics (`v0.1`)

- **Serial console** — COM1 115200 8N1 with a scratch-register probe, so a
  machine without a UART cannot hang the kernel
- **Structured logging** — elapsed-time prefix, level filter, module tag,
  swappable sink, bounded `vsnprintf`
- **On-screen console** — the boot log mirrored to the screen
- **Panic path** — interrupts off first, recursion guard, reason/file/line/function,
  full register dump including `CR2` for page faults. Never reboots

#### Tooling (`v0.1`)

- **`tools/build_toolchain.sh`** — `x86_64-elf` and `aarch64-elf` GCC
  cross-compilers, idempotent so a partial run resumes. No libstdc++ for the
  `-elf` targets
- **`tools/mkimage.py`** — bootable GPT image with a FAT32 ESP, pure Python.
  Boot sector, FSInfo, FAT, long-name entries, multi-cluster files, directory
  growth, primary and backup GPTs with correct CRCs
- **`tools/verify_image.py`** — independent structural verifier, 35 checks
- **`tools/run_qemu.py`** — interactive boot and a `--test` mode for CI

#### Testing & CI (`v0.1`)

- **25 host tests** over the FAT32 geometry solver, the writer, the GPT builder,
  the image pipeline and the shared boot constants. Includes a byte-exact
  round trip through the FAT32 writer and a negative test asserting the verifier
  rejects a deliberately corrupted GPT
- **In-kernel self tests** — strings, `vsnprintf`, every boot-handoff rejection
  path, framebuffer invariants
- **GitHub Actions** — host tests, then cross-compile, 64 KiB `.text` budget
  gate, image packaging and verification, QEMU boot with marker assertions, plus
  a lint job that fails on committed artefacts or credentials

#### Documentation

- **Complete engineering blueprint** — `docs/AfriyieOS-Blueprint.md`
- `docs/architecture/` — boot flow, memory model, IPC protocol, capability model
- `docs/abi/` — syscall table, the AFS on-disk format
- `docs/design/` — the UI design system, the brand guide
- `docs/debug-log.md` — seven v0.1 bugs with root causes and lessons
- `docs/contributing.md`, `docs/README.md`, `docs/releases/v0.1.0.md`
- Repository scaffolding: `README.md`, `LICENSE` (MIT), `.gitignore`,
  `.gitattributes`, `CHANGELOG.md`

### Fixed

- **UEFI pixel format mapping was inverted.** UEFI names formats by byte order in
  memory; `af_pixel_format_t` names the little-endian integer layout. The first
  draft would have swapped red and blue on every desktop
- **`mov rsp, kernel_stack_top`** would have loaded the zero stored at that `.bss`
  address instead of its address — an immediate triple fault. Now `lea`
- **FAT32 directory writer stopped at the `0x00` terminator** instead of
  continuing until it had enough slots, so a multi-entry long-name chain landing
  in a directory's last free slot could not be written. Found by a test that
  writes 200 files into one directory
- **A 32 MiB ESP cannot be FAT32 at any cluster size.** The builder now refuses it
  with an explanation rather than emitting a file system firmware would not mount
- **`.gitignore` matched a bare `core`**, swallowing the entire `kernel/core`
  source directory

### Notes

- The v0.1 kernel is an identity mapping at 1 MiB, not in the higher half: a
  higher-half image faults on its first instruction fetch without paging. The
  move is coupled to the v0.2 paging work
- The ARM64 phone port is scheduled for v1.1 by ADR-007, so the HAL is validated
  on one architecture before being duplicated

---

## Milestone index

| Version | Codename | Theme | Status |
| --- | --- | --- | --- |
| v0.1 | Seed | Boot & display | 🔨 code complete, awaiting green CI |
| v0.2 | Roots | Memory & multitasking | ⏳ planned |
| v0.3 | Trunk | Disk & file system | ⏳ planned |
| v0.4 | Branches | User mode & syscalls | ⏳ planned |
| v0.5 | Leaves | Drivers & input | ⏳ planned |
| v0.6 | Bloom | Graphics core | ⏳ planned |
| v0.7 | Fruit | IPC & user-space services | ⏳ planned |
| v0.8 | Canopy | Window manager & shell | ⏳ planned |
| v0.9 | Harvest | Installer & images | ⏳ planned |
| v1.0 | Oseadeɛyɔ | Installable & usable | ⏳ planned |
| v1.1 | Twin | ARM64 phone port | ⏳ planned |
| v1.2 | Dual | Acceleration & polish | ⏳ planned |
| v2.0 | Horizon | Networking & ecosystem | ⏳ planned |

---

[Unreleased]: https://github.com/hayfordafriyie/AfriyieOS/commits/main
