# Contributing to AfriyieOS

---

## 1. The one rule

> **`main` is always green and always bootable.**

If a change breaks the build or the boot, fixing it is the highest priority in the
project — above any feature. A `main` that sometimes does not boot is a `main`
nobody trusts, and a project nobody trusts stops being worked on.

---

## 2. Getting set up

### 2.1 Host

Ubuntu 22.04+ (or WSL2 on Windows — build and run inside WSL; edit and commit
from either side).

```bash
sudo apt install build-essential bison flex libgmp3-dev libmpc-dev \
                 libmpfr-dev texinfo nasm xorriso mtools \
                 qemu-system-x86 qemu-system-arm qemu-utils gdb \
                 python3 python3-pip cmake ninja-build git wget curl
```

### 2.2 Cross-compiler

```bash
./tools/build_toolchain.sh              # both targets; 20-40 minutes, once
export PATH="$HOME/opt/cross/bin:$PATH"
```

The script is idempotent, so re-running after a failure resumes rather than
restarting.

### 2.3 Build and run

```bash
cmake -B build/x86_64 -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-x86_64-elf.cmake \
      -DAF_TARGET=x86_64

cmake --build build/x86_64
python3 tools/run_qemu.py --arch x86_64 --image build/x86_64/afriyieos.img
```

Quit QEMU with **Ctrl-A then X**.

---

## 3. Before you push

Run these. CI runs the same things, and finding a failure locally is much faster.

```bash
# Host tests — fast, no cross-compiler needed
python3 -m unittest discover -s tests/host -v

# Build, with warnings as errors
cmake --build build/x86_64

# Kernel size against the 64 KiB .text budget
cmake --build build/x86_64 --target kernel-size

# Package and verify the image
python3 tools/mkimage.py --arch x86_64 --build-dir build/x86_64 \
        --output build/x86_64/afriyieos.img
python3 tools/verify_image.py --image build/x86_64/afriyieos.img \
        --expect "EFI/BOOT/BOOTX64.EFI:build/x86_64/boot/BOOTX64.EFI"

# Boot it and assert the markers
python3 tools/run_qemu.py --arch x86_64 \
        --image build/x86_64/afriyieos.img --test --timeout 90
```

---

## 4. Debugging

| Need | How |
| --- | --- |
| See the boot log | `--serial stdio` (the interactive default) |
| Break early | `run_qemu.py ... --debug` — halts at reset, GDB stub on `:1234` |
| Attach GDB | `gdb -ex 'target remote :1234' build/x86_64/kernel` |
| Screenshot | QEMU monitor: `screendump shot.ppm` |
| Inject input | QEMU monitor: `sendkey a`, `mouse_move 100 200`, `mouse_button 1` |
| Inspect the image | `python3 tools/verify_image.py --image <img>` |
| Kernel memory map | `build/x86_64/kernel.map` |
| Section sizes | `x86_64-elf-size -A build/x86_64/kernel` |

**When a boot hangs with no output**, bisect. It is the only technique that always
works: comment out half the boot path, see which half, repeat. Guessing at a
silent triple fault is how you lose a night.

When you fix a non-trivial bug, **add an entry to
[docs/debug-log.md](debug-log.md)**. The format is at the top of that file.
Future-you is the primary beneficiary.

---

## 5. Code standards

### 5.1 Kernel (C11)

```c
/* Naming */
snake_case for functions and variables
UPPER_SNAKE for macros and constants
struct foo { ... }; typedef struct foo foo_t;   /* _t for types */

/* Rules */
- Every fallible function returns af_status_t. Never a magic value.
- Every allocation is checked. "It cannot fail here" is not true in a kernel.
- No VLAs, no recursion in kernel paths, no floating point outside an explicit
  FPU context save.
- No dynamic allocation in interrupt context. Ever.
- All MMIO through af_mmio_read32 / af_mmio_write32. Never a bare dereference:
  the compiler may reorder, widen or eliminate plain loads and stores.
- Shared state carries a comment naming its locking discipline.
- Functions over ~60 lines get split, except unrolled page-table walkers.
- Every file starts with an SPDX line and a short note on what it is for.
```

### 5.2 User space (C++20)

```
Allowed :  templates, constexpr, namespaces, classes, RAII, operator overloading
Banned  :  exceptions, RTTI, std::vector, std::string, iostream, new/delete,
           dynamic_cast, typeid, thread_local
Replaced:  af::Vector<T>, af::String, af::HashMap<K,V>, af::UniquePtr<T>,
           af::Arena, af::Result<T>
```

Compiled with `-fno-exceptions -fno-rtti -nostdlib++ -nostdinc++`. There is no
hidden allocation and no hidden control flow.

### 5.3 Comments

Comment the **why**, not the **what**.

```c
/* Bad: increments i */
i++;

/* Good: the last descriptor is a terminator, so stop one early — reading it
 * would be a wild pointer dereference that only shows on the final iteration. */
i++;
```

Every non-obvious constant, every workaround, and every ordering that is
load-bearing gets a comment. `entry.asm` and `idt.c` are the models to follow.

### 5.4 Warnings

`-Wall -Wextra -Werror` and a long list of specific warnings are on. **Never**
silence a warning with a cast or an unused-variable pragma to make a build pass.
A warning is a question the compiler is asking; answer it.

---

## 6. Commits

Conventional commits, one logical change each. The commit + push after every
meaningful change is expected — the history should read like real, incremental
engineering.

```
feat(kernel):     a new capability
fix(vmm):         a bug fix
docs:             documentation only
build:            CMake, toolchains, flags
test:             tests only
refactor:         no behaviour change
chore:            tooling, gitignore, metadata
perf:             a measured improvement
```

A good message:

```
pmm: allocate contiguous runs with a rotating hint

best-fit from the last successful allocation rather than from block 0.
Without the hint every allocation scans the whole bitmap, which is the
difference between 40 cycles and 40 000 on a fragmented 16 GiB machine.

Adds pmm_alloc_frames() and pmm_free_frames() with the same refcounting
discipline as the single-frame path.
```

A message that would be rejected:

```
fixed stuff
```

### 6.1 Never commit

Build output, images, `.elf`/`.bin`/`.efi`, `__pycache__`, editor directories, or
**credentials**. CI fails the build if any of these appear in the tree. The
gitignore covers them, but check `git status` before a wide `git add -A`.

---

## 7. Testing expectations

Match the test to the tier (blueprint §12.1):

| Change | Required |
| --- | --- |
| Kernel algorithm (allocator, layout solver, FAT/AFS parsing) | T1 or T2 unit test |
| Anything on the boot path | A boot marker, and a T3 assertion |
| Image or tooling format | A host test round-tripping the data |
| A crash fix | An entry in the debug log |
| A performance-sensitive path | A measurement in the release notes |

A bug fix without a regression test is a bug that will come back. If the bug was
found by a test, the test already exists — keep it. If it was found by hand, write
the test that would have caught it.

When adding a marker to the kernel, add it to `EXPECTED_MARKERS` in
`tools/run_qemu.py` in the same commit, so CI enforces it.

---

## 8. Performance discipline

The budgets in blueprint §14 are gates, not aspirations:

| Metric | Budget |
| --- | --- |
| Kernel `.text` | < 64 KB (enforced in CI) |
| Context switch | < 1 000 cycles |
| IPC round trip | < 2 000 cycles |
| Null syscall | < 300 cycles |
| Boot to shell | < 5 s in QEMU |
| Compositor frame (4 windows, 1080p) | < 16.6 ms |

If a change breaks a budget, it does not merge — unless the change is
accompanied by an ADR explaining why the budget was wrong. "It got slow" is a
bug report, not a trade-off.

---

## 9. Working on the roadmap

Pick a milestone from blueprint §11 and work through its checkboxes in order. Do
not start a later milestone before an earlier one's acceptance criteria pass:
each one exists to de-risk the next, and skipping ahead means discovering a
foundational problem three milestones too late to fix cheaply.

Update the checklist in the same commit as the work. A plan that drifts from
reality stops being read.

---

## 10. References

* [AfriyieOS-Blueprint.md](AfriyieOS-Blueprint.md) — the specification
* [debug-log.md](debug-log.md) — bugs already solved, and their lessons
* [architecture/boot-flow.md](architecture/boot-flow.md) — where to start reading the code
* [README.md](README.md) — the documentation index
* OSDev Wiki — <https://wiki.osdev.org>
