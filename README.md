# AfriyieOS

> **A minimal, modern, cross-platform operating system for PCs (x86_64) and Phones (ARM64) — built from scratch.**

AfriyieOS is a **capability-based microkernel operating system**. The kernel is deliberately tiny — it only schedules threads, passes messages, manages virtual memory, and routes interrupts. Everything else (drivers, file system, network, window manager, applications) runs as isolated user-space processes. That isolation is what lets one codebase target both a UEFI desktop PC and an ARM64 phone.

[![MIT License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Status](https://img.shields.io/badge/status-v0.1%20in%20development-orange.svg)](docs/AfriyieOS-Blueprint.md)
[![Architecture](https://img.shields.io/badge/arch-x86__64%20%7C%20aarch64-informational.svg)](docs/AfriyieOS-Blueprint.md)

---

## 📖 Start here

The complete engineering specification lives in one document:

### ➡️ **[docs/AfriyieOS-Blueprint.md](docs/AfriyieOS-Blueprint.md)**

It contains the full architecture, language and framework choices, the kernel ABI, the complete `v0.1 → v2.0` version roadmap, and a step-by-step TODO breakdown for every milestone through installation and daily use.

---

## 🏛️ Architecture at a glance

```
┌─────────────────────────────────────────────────────────┐
│  APPS        Terminal · Notes · Files · Settings        │
├─────────────────────────────────────────────────────────┤
│  UI          Afriyie Shell · responsive PC/phone layout │
├─────────────────────────────────────────────────────────┤
│  SERVICES    Compositor · FS · DevMgr · Net · Audio     │
├─────────────────────────────────────────────────────────┤
│  DRIVERS     virtio · NVMe · USB HID · eMMC · I²C touch │
├═════════════════════════════════════════════════════════┤
│  ABI         syscalls · IPC endpoints · capabilities    │
├═════════════════════════════════════════════════════════┤
│  MICROKERNEL scheduler · IPC · virtual memory · IRQs    │
│              target: < 64 KB of code                    │
├─────────────────────────────────────────────────────────┤
│  HAL         x86_64: GDT/IDT/PML4 · ARM64: VBAR/TTBR    │
├─────────────────────────────────────────────────────────┤
│  BOOT        UEFI (PC) · U-Boot + boot.img (Phone)      │
└─────────────────────────────────────────────────────────┘
```

## 🛠️ Technology

| Layer | Language / Tool |
| --- | --- |
| Kernel, HAL, memory, IPC | **C11** |
| CPU init, context switch, ISR stubs | **NASM** (x86_64) / **GNU as** (ARM64) |
| Drivers, services, UI framework | **C++20** (`-fno-exceptions -fno-rtti`, no STL) |
| Boot bridge | **C11** + UEFI / U-Boot |
| Vector rendering | **ThorVG** (vendored) |
| Build | **CMake 3.28 + Ninja**, GCC cross-compilers (`x86_64-elf`, `aarch64-elf`) |
| Tooling & CI | **Python 3.11**, QEMU, GitHub Actions |

## 🗺️ Roadmap

| Version | Codename | Deliverable |
| --- | --- | --- |
| **v0.1** | Seed | Boots to a framebuffer splash in QEMU |
| **v0.2** | Roots | Paging + preemptive multitasking |
| **v0.3** | Trunk | Reads files from disk through FAT32 |
| **v0.4** | Branches | User mode, syscalls, ELF loader |
| **v0.5** | Leaves | Device enumeration, keyboard/mouse input |
| **v0.6** | Bloom | 2D graphics engine and compositor |
| **v0.7** | Fruit | IPC, capability system, user-space services |
| **v0.8** | Canopy | Window manager and Afriyie Shell |
| **v0.9** | Harvest | Installer, AFS file system, bootable images |
| **v1.0** | **Oseadeɛyɔ** | **Installable and usable: terminal, notes, files, settings** |
| v1.1 | Twin | ARM64 phone port |
| v1.2 | Dual | GPU acceleration, SMP, power management |
| v2.0 | Horizon | Networking, package manager, app ecosystem |

## 🚦 Current status

**v0.1 — Seed: in development.** No bootable image yet; the specification is complete and the source tree is being built out.

## 🤝 Building (once v0.1 lands)

```bash
# 1. Build the cross-compilers (Linux / WSL2)
./tools/build_toolchain.sh
export PATH="$HOME/opt/cross/bin:$PATH"

# 2. Configure and build
cmake -B build/x86_64 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-x86_64-elf.cmake
cmake --build build/x86_64

# 3. Boot it in QEMU
python3 tools/run_qemu.py --arch x86_64 --image build/x86_64/afriyieos.img
```

## ⚠️ A note on scope

Building an operating system is a multi-year project. AfriyieOS is structured so that **every version is independently demonstrable** — you can boot it, see it work, and show it to someone. That is what makes the long middle survivable.

See [§1.3 Reality Check](docs/AfriyieOS-Blueprint.md#13-reality-check--read-this-before-writing-a-line-of-code) in the blueprint for honest effort estimates.

## 📄 License

[MIT](LICENSE) © Hayford Afriyie
