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

## 2025 — Entries from v0.2 development

### `context_switch` saved `rsp` eight bytes past the return address

**Milestone:** v0.2
**Symptom:** an invalid-opcode fault with `RIP` pointing **inside `.bss`** — a
control transfer to a data address — a few hundred context switches into the
scheduler acceptance test. Nothing looked wrong before it: both threads had been
created, the timer was ticking, the idle thread was running.
**Cause:** the saved stack pointer was off by one slot. `context_switch` ended
with `ret`, which pops the *incoming* thread's return address, so the saved `rsp`
for the outgoing thread was recorded as `rsp + 8` — skipping its own return
address. On resume, `mov rsp, [ctx+CTX_RSP]` followed by `ret` therefore popped
whatever sat in the slot *above* the return address, and jumped there.

The comment justifying it read: *"The return address is on our stack, and `ret`
below will consume it from the incoming stack. So we must pop it ourselves and
record rsp AFTER the pop, otherwise the resumed thread would return into a stale
frame."* Every clause is individually true and the conclusion is wrong. The
outgoing thread's return address must **stay** on its stack, because that is
exactly what its own `ret` will pop when it is next resumed.
**Fix:** `mov [rdi + CTX_RSP], rsp` — no arithmetic. The convention is the one
`call`/`ret` already uses: `rsp` points *at* the return address. Both cases then
work unchanged, including a brand new thread, whose stack has
`thread_trampoline` sitting in exactly that slot.
**Found by:** `tools/resolve_addr.sh` turning the faulting `RIP` into
`kernel_stack_bottom + 0x3858` — a *data* symbol — which meant a corrupted
control transfer rather than a bad instruction. That ruled out the decoder, the
handler and the code being executed, and left the saved context as the only
remaining suspect.
**Lesson:** this is the bug the blueprint warns about in as many words — *"you
will spend 99% of your time debugging context switch register save errors"* —
and it did not present as a register being wrong. It presented as a jump to
nonsense several hundred switches later, in code with no connection to the
scheduler. Two things made it findable: the panic dump prints every register, and
resolving an address to a symbol costs seconds. A wrong-looking-but-plausible
comment is worth less than a test that runs long enough to expose the failure.

---

### The v0.2 acceptance test did not test what it claimed

**Milestone:** v0.2
**Symptom:** after the context-switch fix, both threads ran to completion but the
test failed with *"only 2 A/B transitions in 400 entries — the threads ran
sequentially"*.
**Cause:** the test was wrong, not the scheduler. One worker yielded once per
iteration; the other was meant to be preempted. But 200 iterations of a trivial
loop complete in **microseconds**, far inside one 10 ms time slice, so the second
worker finished its entire workload before the timer ever had a chance to
interrupt it. The scheduler was behaving correctly; the test had never created the
conditions it claimed to be testing.
**Fix:** two separate tests, each of which actually exercises its mechanism.
Both workers now yield, producing a near-strict alternation that is directly
observable (the run shows 201 switches each). Preemption is tested with a thread
that never yields and never sleeps: if the boot thread runs again while that
thread is going, the CPU *must* have been taken away by the timer. It counted
**13.8 million iterations** while the boot thread slept, which is a proof rather
than a correlation — no cooperative mechanism could produce that result.
**Lesson:** a failing test is a claim about the system, and it can itself be the
thing that is wrong. Before changing the code to satisfy a test, check that the
test sets up the conditions it asserts about. The first diagnosis here —
"the scheduler starved them" — pointed at entirely the wrong subsystem, and
acting on it would have meant debugging working code.

---

### A created thread was never put on a run queue

**Milestone:** v0.2
**Symptom:** *"scheduler test: alpha reached 0 of 200 iterations — the scheduler
starved it"*. Both threads existed; neither ever executed.
**Cause:** `thread_create()` built the thread, allocated its stack, set up its
context — and left it in state `CREATED` forever, because nothing put it on a run
queue. `sched_admit()` did not exist.
**Fix:** an explicit admission step, called by whoever creates the thread,
documented with the failure mode spelled out. `thread_create()` does not admit
itself because it holds the thread-table lock and the scheduler has its own lock
ordering.
**Found by:** the thread dump showing two threads stuck in `CREATED` while the
scheduler reported an empty run queue. `thread_dump_all()` earns its keep here.
**Lesson:** "created" and "runnable" are different states, and the gap between
them is invisible unless something prints it. State machines need a dump.

---

### `sched_yield` held a spinlock across a context switch

**Milestone:** v0.2
**Symptom:** the machine stopped dead with no fault and no output, immediately
after the idle thread started.
**Cause:** `sched_yield()` took a spinlock, called `context_switch()`, and
released the lock afterwards. Thread A takes the lock, switches to B; B calls
`sched_yield()`, tries the same lock, and spins forever waiting for a thread that
is not running. `af_spin_lock` also disables interrupts, so the machine stops
taking ticks too.
**Fix:** no lock across a switch. Interrupts are disabled only around the run-queue
mutation and re-enabled *before* the switch, so the machine is taking ticks again
by the time the new thread runs.
**Lesson:** a lock held across a context switch is meaningless, because its holder
is not on a CPU any more. The only correct protection for a critical section that
ends in a switch is disabling interrupts for the part that touches shared state,
and re-enabling them before the switch.

---

### A brand new thread inherited interrupts disabled

**Milestone:** v0.2
**Symptom:** would have been a system that froze the instant a second thread
started — no ticks, no preemption, no output.
**Cause:** a thread entered from inside the timer interrupt starts with `IF`
cleared, because the interrupt gate clears it. A *resumed* thread is fine: it
returns through its own `iretq`, which restores its flags. A thread that has never
run has no earlier state to restore.
**Fix:** `thread_trampoline` executes `sti` before calling the entry function.
**Lesson:** the two ways a thread begins — resumed versus brand new — differ in
exactly the places where the CPU restores state automatically for one and not the
other. Flags, FPU state and segment registers all fall into this category.

---

### The boot context *was* the idle thread

**Milestone:** v0.2
**Symptom:** the acceptance test spun 2000 times without ever yielding, and
reported that the scheduler had starved both worker threads.
**Cause:** `kmain`'s context was adopted as the idle thread. `sched_sleep()` and
`sched_block()` deliberately refuse to act on the idle thread — blocking it would
deadlock the machine — so every sleep in the boot path silently became a no-op.
**Fix:** the two are separate threads. `sched_init()` now takes both, and the idle
thread is created like any other.
**Lesson:** a safety check that silently does nothing is a trap. The guard was
correct; returning quietly instead of reporting made it invisible. There is a
case for an `AF_ASSERT` there, at least in debug builds.

---

### The PMM freed the kernel's own `.bss`

**Milestone:** v0.2
**Symptom:** a general protection fault inside `pmm_alloc_frames`, reading the
bitmap. The register dump showed `rax = 0x1818181818181818` — a fill pattern the
heap self test had just written — as the value of `s_bitmap`.
**Cause:** the boot bridge reports the size of the **flat binary** it copied, and
`objcopy -O binary` does not emit NOBITS sections. So `kernel_phys_size` covered
`.text`, `.rodata` and `.data`, and silently omitted `.bss` — 111 KiB of real
memory that the kernel was using at that moment. `pmm_init` reserved only the
reported range, the free pass released the rest, and the heap then allocated the
kernel's own `.bss` and filled it with a test pattern.
**Fix:** two deliberately redundant ones. `objcopy` now materialises `.bss` with
`--set-section-flags .bss=alloc,load,contents`, so the reported size is honest;
and `pmm_init` independently reserves the kernel's full extent from the linker's
`__kernel_start`/`__kernel_end` symbols, taking whichever figure is larger.
**Found by:** resolving `0x1068fe` with `nm` to `pmm_alloc_frames+0x85`, then
disassembling to see it was `movzbl (%rax),%edx` — a read through the corrupted
`s_bitmap`.
**Lesson:** the crash was three function calls from the cause, which is how
allocator bugs behave. Two things made it findable at all: the panic dump printed
every register, and `tools/resolve_addr.sh` turned an address into a symbol. Also
worth noting: **two complementary fixes, not one.** Either alone would have
worked; the failure mode is silent corruption, and redundancy is cheap here.

---

### The frame bitmap was sized from the highest address in the map

**Milestone:** v0.2
**Symptom:** none — it booted. But the log said `managing 268435456 frames
(1048576 MiB) with 294912 KiB of metadata` on a machine with 2 GiB of RAM, and
only 448 952 frames were free.
**Cause:** `pmm_init` sized the bitmap from `af_boot_info_max_address()`, the
highest address anywhere in the firmware's memory map. Firmware routinely
describes device and reserved windows far above RAM — QEMU's map reaches **1 TiB**
on a 2 GiB machine — so the bitmap covered 1 TiB of address space that can never
be allocated from. 32 MiB of bitmap plus 256 MiB of refcounts: **288 MiB of
metadata, 14% of the machine**, and initialisation loops walking 268 million
frames.
**Fix:** `af_boot_info_max_usable_address()`, which considers only regions the PMM
may ever allocate from. Metadata fell from 288 MiB to 576 KiB, free memory rose
from 448 952 to 522 509 frames — **287 MiB recovered** — and the largest
contiguous free run went from 1713 MiB to 2001 MiB.
**Found by:** reading the log numbers and noticing that "1 TiB" and "2 GiB" were
in the same line. Nothing failed; the figures were simply absurd, and absurd
figures in a boot log are worth stopping for.
**Lesson:** "it works" is not the same as "it is right". A sizing bug that wastes
14% of RAM and makes every scan 500× longer passes every functional test. The
habit that catches it is reading your own output critically rather than just
checking that it appeared.

---

### A header's own last field was overwritten by the offset used to find it

**Milestone:** v0.2
**Symptom:** `heap self test: large allocation reports 16 usable bytes` for a
100 KiB allocation.
**Cause:** large allocations kept their header at the start of the block and
stored, in the four bytes immediately below the returned pointer, the distance
back to that header — so `kfree` could find it with one subtraction. When the
allocation is not alignment-shifted, that distance *is* `sizeof(header)`, and
those same four bytes are where the header's **last field** lives. Storing the
offset overwrote `requested` with the value 16.
**Fix:** the trailer is now at a fixed offset (`sizeof(large_trailer_t)`) before
the user pointer and records the block base directly. No offset field, no
arithmetic that can collide with the thing it describes.
**Found by:** the self test checking `kmalloc_usable_size` against what was
requested. Both figures were in the failure message, which is what made the
"16" immediately recognisable as `sizeof(header)`.
**Lesson:** writing bookkeeping into the very bytes it is meant to describe is
the kind of mistake that compiles cleanly, reads as clever, and destroys data
later. Prefer a fixed offset to a computed one whenever the fixed one is
available.

---

### `compile_check.sh` used `-fsyntax-only` and missed `-Wunused-function`

**Milestone:** v0.2 (tooling)
**Symptom:** the compile check reported all 18 files clean; the real cross build
then failed on `mark_range_used defined but not used`.
**Cause:** `-fsyntax-only` stops after parsing and semantic analysis and never
runs the whole-translation-unit passes that report unused functions and
variables.
**Fix:** the check now compiles to an object with `-c`, exactly as the build
does. It costs a fraction of a second more and predicts the build instead of
approximating it.
**Found by:** the tool disagreeing with the compiler, which is the only thing
that makes a pre-flight check worth having.
**Lesson:** a check that predicts the build must actually perform the build's
work. An approximation that passes when the build fails trains you to ignore it.

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
