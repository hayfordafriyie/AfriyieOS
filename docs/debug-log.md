# AfriyieOS Debug Log

Every non-trivial bug, its root cause, and how it was found.

This file is the most valuable document in the repository by `v0.4`, because the
second time you meet a fault you have already solved it once. Write the entry
even when the fix was one character — **especially** when the fix was one
character, because that is the kind of bug you will write again.

## Entry format

```
### YYYY-MM-DD — short title
Milestone:  vX.Y
Symptom:    what was observed
Cause:      the actual root cause, not the first suspicion
Fix:        the change
Found by:   how it was located (bisection? static assert? reading the spec?)
Lesson:     the generalisable part
```

---

## 2025 — Entries from v0.1 development

### `mov` loaded the stack symbol's contents instead of its address

**Milestone:** v0.1
**Symptom:** would have been an immediate triple fault at the first `call`, with
no output whatsoever — the machine simply resets.
**Cause:** `mov rsp, kernel_stack_top` in NASM loads the 8 bytes **stored at**
the address `kernel_stack_top`. That symbol lives in `.bss`, which the very next
instructions are about to zero, so `rsp` would have become `0`.
**Fix:** `lea rsp, [rel kernel_stack_top]` — take the address, do not read it.
**Found by:** reading the entry stub line by line before ever building it.
**Lesson:** NASM's symbol syntax is address-by-value, not address-by-reference.
Every load of a linker-provided symbol needs a deliberate decision: `mov` to read
memory, `lea` to compute an address. This is the single most common first-boot
bug in x86 kernels.

---

### UEFI pixel format mapping was inverted

**Milestone:** v0.1
**Symptom:** would have produced a red/blue swap on every desktop machine —
a splash screen in the wrong colours, with everything else working perfectly.
**Cause:** UEFI names its pixel formats by **byte order in memory**
(`PixelBlueGreenRedReserved8BitPerColor` means memory bytes B, G, R, X). AfriyieOS
names its formats by the **little-endian integer layout** a 32-bit read produces
(`AF_PIXEL_RGBX8888` means the value `0x00RRGGBB`). These are inverses of each
other, and the bridge mapped them the wrong way round.
**Fix:** the mapping is now
`PixelBlueGreenRed → AF_PIXEL_RGBX8888` and
`PixelRedGreenBlue → AF_PIXEL_BGRX8888`, with the integer layout written out
explicitly in `boot_info.h` so the next reader does not have to rediscover it.
The `PixelBitMask` case now derives the layout from the masks instead of
assuming.
**Found by:** writing the comment that was supposed to justify the code, and
finding it did not.
**Lesson:** when two systems name the same thing from opposite ends, write the
integer layout down next to the enum. "BGR" alone is ambiguous; `0x00BBGGRR` is
not.

---

### FAT32 directory writer could not write a long-name chain at a directory boundary

**Milestone:** v0.1 (tooling)
**Symptom:** `FAT32 error: directory has 2 free slots but 3 entries must be
written`. Reproduced by writing 200 files named `file-NNNN.txt` into a single
directory.
**Cause:** the slot collector stopped as soon as it found the `0x00` terminator,
collected everything from that slot to the end of the cluster, and then stopped
extending the chain. A long file name needs two long-name entries plus the real
entry — three slots. When only two were free, there was nowhere to put the third.
**Fix:** the loop now continues until it has **enough slots**, extending the
cluster chain when it runs out, rather than stopping at the first terminator.
**Found by:** the `test_many_files_in_one_directory` host test. It was written
specifically because a single-file image would never have hit it.
**Lesson:** "stop when the terminator is found" and "stop when there is enough
room" are different conditions, and the dif-ference only appears at a boundary.
Boundary-shaped tests find these; happy-path tests do not.

---

### A 32 MiB EFI System Partition cannot be FAT32

**Milestone:** v0.1 (tooling)
**Symptom:** `volume has only 64 480 clusters; FAT32 requires at least 65 525`.
**Cause:** FAT32 is defined as a range: at least 65 525 clusters and at most
`0x0FFFFFF5`. At one 512-byte sector per cluster — the only sensible choice on a
small volume — 32 MiB does not yield enough cluster-sizes-worth of sectors. No
cluster size rescues it, because a larger cluster only reduces the count further.
**Fix:** the builder raises with an explanation. The minimum practical ESP is
about 33.5 MiB at 512-byte clusters, and the default is 64 MiB. The constraint is
documented on `ESP_SIZE_BYTES` so nobody re-derives it.
**Found by:** the `test_geometry_converges_for_a_range_of_sizes` test.
**Lesson:** FAT32 is not "any small FAT volume". The 65 525-cluster floor is a
hard definitional boundary, and the obvious modern default of 4 KiB clusters
makes a 64 MiB volume *invalid*. Small volumes need small clusters.

---

### GPT header CRC verification always failed on a correct image

**Milestone:** v0.1 (tooling)
**Symptom:** the image builder produced an image, and the verifier rejected it:
`header CRC32 matches (stored 0x85944976, computed 0xF8F58709)`.
**Cause:** the verifier's bug, not the writer's. A GPT header CRC is computed
over the header bytes with the CRC32 field itself **zeroed**. Computing it over
the stored bytes — checksum included — can never match.
**Fix:** `gpt_header_crc()` copies the header, zeroes bytes 16..20 and hashes
that.
**Found by:** suspecting the writer first, then reading the specification for the
CRC procedure rather than assuming it was the same as the entry-array CRC (which
*is* computed over the stored bytes, which is exactly why the mistake was easy to
make).
**Lesson:** when a checksum disagrees, check the *verifier* before the producer.
Two different checksums in the same structure can legitimately have different
procedures.

---

### `.gitignore` swallowed the entire kernel source directory

**Milestone:** v0.1
**Symptom:** `git add kernel/core` reported "The following paths are ignored by
one of your .gitignore files: kernel/core".
**Cause:** the ignore file contained a bare `core`, intended for core dumps. A
`.gitignore` pattern without a slash matches a path component at any depth, so it
matched the directory `kernel/core` too.
**Fix:** the core-dump patterns are now explicit (`core.[0-9]*`, `core.*.dmp`),
with a comment explaining why a bare `core` is dangerous here.
**Found by:** the commit failing loudly, which is at least a good failure mode.
**Lesson:** never use a bare directory name as a `.gitignore` pattern when that
name might plausibly be a source directory. `core`, `build`, `target`, `bin` and
`doc` all bite.

---

### PIC remapping is not optional

**Milestone:** v0.1 (caught by design, recorded because it will be met again)
**Symptom:** would be a double fault on the first timer tick.
**Cause:** at reset the 8259 PICs deliver IRQ0..7 as vectors `0x08`..`0x0F`, which
collide exactly with the CPU exception vectors. The first timer interrupt would
be dispatched as `#DF`.
**Fix:** remap the master to `0x20` and the slave to `0x28` during IDT setup.
**Found by:** reading `idt.c` against the exception table it defines.
**Lesson:** the default hardware state is almost never the state you want. Every
remap in the boot path is load-bearing, and worth a comment saying so.

---

## Open questions

Things that are suspected but not yet confirmed. Move them up into a real entry
once they bite — or delete them once the code proves them harmless.

- **QEMU `virtio-blk-pci` with both `if=none` and `-drive`** — the current
  `run_qemu.py` attaches the disk with `-device virtio-blk-pci,drive=bootdisk`
  and `-drive ...if=none,id=bootdisk`. This is the modern syntax and works, but
  it is untested against a real boot until CI runs.
- **OVMF paths across distributions** — five candidate paths are tried. If a
  distribution lays OVMF out differently, `run_qemu.py` exits with a clear
  message rather than guessing.
- **`o64 retf` in `cpu.asm`** — NASM's spelling of a 64-bit far return. If the
  assembler version in CI rejects it, `db 0x48, 0xCB` is the equivalent
  encoding.
- **Higher-half kernel** — not enabled at v0.1. When it is enabled in v0.2, the
  failure mode of getting it wrong is an instant triple fault with no output, so
  the switch will be made in a commit that also adds the paging bootstrap.
