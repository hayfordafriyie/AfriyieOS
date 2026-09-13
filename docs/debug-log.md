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

### `EFI_SYSTEM_TABLE` was packed, so `ConOut` was read from the wrong offset

**Milestone:** v0.1
**Symptom:** none yet — caught before it ran. It would have been an instant
triple fault on the boot bridge's very first console write, with no output
whatsoever.
**Cause:** two mistakes compounding. The structure was marked `AF_PACKED` *and*
its asserted member offsets were computed for a packed layout. UEFI firmware is
compiled as ordinary C, so its structures use **natural alignment**: after the
24-byte table header and the `u32 FirmwareRevision` there are four bytes of
padding before `ConsoleInHandle`, which puts `ConOut` at offset **64**, not 60.
**Fix:** `AF_PACKED` removed from every structure the firmware produces — packing
is for *our* formats, which we define byte for byte — and every member offset
asserted individually, with the reasoning written at the top of `boot/uefi/efi.h`.
**Found by:** the static asserts firing on the first real compile. This is the
one bug in this log that cost nothing, and it is the entire justification for
writing offset asserts against a specification instead of trusting a struct.
**Lesson:** "packed" and "matches the firmware" are not the same thing. A struct
that describes someone else's memory layout must be laid out the way *their*
compiler lays it out.

---

### `kernel_entry` was nowhere near the start of the kernel image

**Milestone:** v0.1
**Symptom:** the boot bridge printed its banner, loaded the kernel, exited boot
services and jumped — then the machine hung with no further output at all. The
serial log simply stopped.
**Cause:** the boot bridge copies the flat kernel binary to physical `0x100000`
and jumps there; it does not read the ELF header. But `kernel_entry` was defined
in a plain `section .text`, so the linker interleaved it with the C functions and
placed it at `0x107900`. The CPU began executing whatever C function happened to
land first, with no stack and no arguments.
**Fix:** the entry stub moved to a dedicated `.text.boot` section, which the
linker script places before everything else, so `kernel_entry` *is* offset zero of
the image by construction. `tools/link_check.sh` and `tools/build.sh` now assert
`kernel_entry == 0x100000`.
**Found by:** `nm build/x86_64/kernel.elf`, after the serial log went quiet and
nothing else could report anything — the failure is past `ExitBootServices`, so
there is no firmware left to blame.
**Lesson:** the deadliest failure mode is the one past the point where anything
can report it. Two defences: make the invariant structural rather than
conventional, and add a build-time assertion so violating it fails the build
instead of the boot. The `ENTRY()` directive in the linker script was set
correctly the whole time and did nothing — it sets the ELF header field, which
nothing here reads.

---

### The FAT32 root directory cluster was never marked allocated

**Milestone:** v0.1 (tooling)
**Symptom:** firmware booted and reported "No bootable option or device was
found". The partition table was fine. `mtools` said `Fat problem while decoding
2 0`; the Linux `vfat` driver said `can't read superblock`.
**Cause:** cluster 2 is the root directory, and the cluster allocator starts
handing out clusters from 3 — so nothing ever wrote a FAT entry for cluster 2. It
read as `0`, meaning "free cluster", and every real FAT driver therefore saw the
root directory's chain as unallocated.
**Fix:** `FAT[2] = 0x0FFFFFFF` at format time. The root directory is a one-cluster
chain, so that is end-of-chain.
**Found by:** running the image through `mtools` and `parted` — implementations
that share no code with mine.
**Lesson:** **this is the one our own verifier could not catch.** `verify_image.py`
walks the same FAT the writer maintains and happily followed a chain the driver
never agreed to. A format is only verified when a *different implementation*
reads it. Reading your own output back proves your reader and writer agree, which
is not the same claim, and is exactly the claim you need.

---

### Console output between `GetMemoryMap` and `ExitBootServices` invalidates the map key

**Milestone:** v0.1
**Symptom:** `ExitBootServices rejected the map key; refetching`, four times, then
`FATAL: ExitBootServices failed after 4 attempts. Halting.`
**Cause:** `ExitBootServices` takes the `MapKey` from `GetMemoryMap` and refuses
the call if the map changed since. It changes whenever anything allocates or frees
a page — and printing to the UEFI console allocates, because the console driver
grows its own buffers. The diagnostic line `memmap: N regions`, placed between
the two calls, invalidated the key every single time.
**Fix:** ordering, not retrying. The map is fetched and `ExitBootServices` is
called back to back with nothing in between, and the console is only used on the
retry path, where the map is about to be re-read anyway.
**Found by:** the retry loop exhausting itself, which forced the question "why
does the key keep changing?" rather than "why does the call keep failing?".
**Lesson:** a retry loop can hide a deterministic bug forever. When a retry
succeeds eventually, ask why it needed a retry at all. The honest answer here was
"because my own diagnostic output broke the precondition" — and the loop would
have concealed it indefinitely if it had happened to succeed on the second
attempt.

---

### The boot handoff pointer arrived in the wrong register

**Milestone:** v0.1
**Symptom:** `boot_info invalid at the supplied pointer; trying the backup
location at 0x7000` / `recovered boot_info from the backup location`. The system
booted anyway.
**Cause:** the boot bridge is compiled with the Microsoft x64 ABI, because that is
what UEFI uses for `efi_main`. Under that ABI the first integer argument goes in
`rcx`. The kernel's entry stub is SysV and reads `rdi`. So the kernel received a
stale value and fell back to the fixed copy the bridge mirrors at `0x7000`.
**Fix:** the kernel entry function pointer is declared `__attribute__((sysv_abi))`,
so the compiler puts the argument where the kernel actually looks for it and
aligns the stack the way SysV requires.
**Found by:** the kernel's own log line, which was written precisely because the
register path is a single point of failure.
**Lesson:** the fallback path paid for itself on the first boot. It converted a
silent hang into a working boot *and* a diagnostic that named the exact problem —
and then the log line it printed is what identified the bug. Build the recovery
path and make it talk.

---

### `efi_console_print` used one index for two buffers

**Milestone:** v0.1
**Symptom:** garbled console text: `AryeS010(ed EIbo rde` instead of
`AfriyieOS 0.1.0 (Seed) UEFI boot bridge`.
**Cause:** the loop used a single index for both the ASCII input and the UTF-16
output buffer. Translating LF into CR+LF advances the output by two and the input
by one, so the two positions desynchronise after the first newline and every
subsequent line is assembled from the wrong characters.
**Fix:** separate `src` and `i` indices.
**Found by:** reading the garbled output closely enough to recognise the intended
string underneath it. The characters were all *there*, which ruled out a memory
problem and pointed straight at an indexing bug.
**Lesson:** "garbled but recognisable" almost always means an off-by-N in a
copy loop, not corruption. Corruption loses data; a desynchronised index
rearranges it.

---

### CMake passed the kernel's GCC flags to NASM

**Milestone:** v0.1
**Symptom:** `nasm: fatal: unrecognised output format 'freestanding'`.
**Cause:** the flags lived on an `INTERFACE` library attached to the whole target,
and CMake applies those compile options to every compiler it invokes — including
NASM, which has its own option syntax and understands none of them.
**Fix:** the options are wrapped in a `$<COMPILE_LANGUAGE:C>` generator
expression. NASM gets its object format from `CMAKE_ASM_NASM_OBJECT_FORMAT`, and
the `-I` and `-D` arguments it *does* accept arrive from
`target_include_directories`/`target_compile_definitions`, which already emit
valid NASM spellings.
**Lesson:** a per-target flag set is not a per-language flag set. Wrap anything
language-specific in a generator expression the moment a target contains more
than one language.

---

### Quotes placed *inside* `-I` and `-Wl,-T` arguments

**Milestone:** v0.1
**Symptom:** `ld: cannot open linker script file "/path/to/bootx64.lds": No such
file or directory` — with the quotes visible in the message. And earlier,
`fatal error: afriyie/types.h: No such file or directory` for a path that
certainly existed.
**Cause:** writing `-I"${dir}"` inside a CMake `COMMAND` passes the quote
characters through to the compiler as part of the argument. The compiler driver
then hands the linker a filename beginning with a double quote.
**Fix:** quote the *whole* argument — `"-I${dir}"`, `"-Wl,-T,${path}"` — so CMake
quotes it as needed and the quotes never reach the program.
**Lesson:** the error message showing the quotes is the tell. When a tool reports
a path with visible quote marks in it, the quoting was done at the wrong level.

---

### `embed.asm.in` used a literal path instead of the CMake placeholder

**Milestone:** v0.1
**Symptom:** `incbin: unable to get length of file 'kernel.bin'`.
**Cause:** the template's fallback was written as a literal `"kernel.bin"` rather
than `"@AF_KERNEL_BIN@"`, so `configure_file` had nothing to substitute and NASM
fell back to a relative path that does not exist in the build directory.
**Fix:** use the `@AF_KERNEL_BIN@` placeholder, with a comment saying what breaks
without it.
**Found by:** dumping the generated file and seeing the fallback text where the
path should have been.
**Lesson:** when a template does not work, look at what it generated. The failure
was in the input file, but the error was in the tool that consumed it.

---

## Open questions

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

## Resolved during the first real boot

Closed out — kept here because the reasoning is worth not re-deriving.

- **`o64 retf` in `cpu.asm`** — ✅ NASM accepted it. The far return reloads `cs`
  correctly, and the GDT is in place before `kmain` runs.
- **OVMF paths across distributions** — ✅ `/usr/share/OVMF/OVMF_CODE_4M.fd` and
  `OVMF_VARS_4M.fd` exist on Ubuntu 24.04. The 4 MiB variants are tried first,
  with the plain names and the edk2 locations as fallbacks.
- **`mov rsp, kernel_stack_top` versus `lea`** — ✅ `lea rsp, [rel ...]` is
  correct and assembles to a fixed 7-byte instruction. Neither form is the bug
  it looks like: NASM treats a bare label as an address and `[label]` as memory,
  so both were "right", but `lea` has a deterministic size, which is what fixed
  the `label changed during code generation` error in `entry.asm`.
- **OVMF's default GOP mode is landscape** — still open for testing purposes.
  Changing it needs a firmware setting rather than a QEMU flag, so the portrait
  splash branch is unverified on screen. The compositor at v0.6 gives us
  resolution control and closes this properly.

## Open questions

Things suspected but not yet confirmed. Move them up into a real entry once they
bite — or delete them once the code proves them harmless.

- **Higher-half kernel** — not enabled at v0.1. When it is enabled in v0.2, the
  failure mode of getting it wrong is an instant triple fault with no output, so
  the switch will be made in a commit that also adds the paging bootstrap. The
  same build-time assertion that caught the entry-point bug should be extended to
  cover the new link address.
- **The COM1 scratch-register probe** — QEMU implements the 16550 scratch
  register, so the probe passes. Real hardware is not guaranteed to, and a UART
  that fails the probe means *silent* loss of all kernel output. Worth revisiting:
  the probe should probably warn loudly rather than silently disabling the
  console.
- **Interrupts stay masked after the PIC remap** — deliberate at v0.1 because
  there are no handlers, but it means `irq_dispatch`'s hardware-IRQ branch is
  untested. v0.3's virtio-blk driver enables the first real IRQ line.
- **`#DF` has no IST stack** — a double fault during the panic path is still a
  triple fault. Needs the TSS, which v0.4 writes.
- **SMP** — the bridge reports `cpu_count = 1` regardless of what the firmware
  says. Real SMP bring-up is v1.2 work; until then `hal_cpu_count()` reads CPUID
  and could disagree with `boot_info`, which nothing currently checks.
