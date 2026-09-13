// SPDX-License-Identifier: MIT
// AfriyieOS — ELF64 loader
//
// =============================================================================
// THE SMALLEST THING THAT IS ACTUALLY AN EXECUTABLE FORMAT
// =============================================================================
// An ELF64 executable is a header, a table of program headers, and some bytes.
// Loading one means:
//
//   1. validate the header — this is data from disk, and a malformed file must
//      be rejected rather than trusted
//   2. for each PT_LOAD segment, map pages at the segment's virtual address with
//      the permissions its flags ask for
//   3. copy p_filesz bytes from the file, and zero the remaining p_memsz -
//      p_filesz bytes — that gap is .bss, and it is *not* in the file
//   4. enter at e_entry
//
// Step 3 is where a loader most often goes quietly wrong. `p_memsz` is larger
// than `p_filesz` for any program with uninitialised globals, and the difference
// is not stored anywhere in the file. A loader that forgets to zero it hands the
// program whatever was in those frames — which on a freshly allocated frame is
// zero by luck, and on a reused one is somebody else's data.
//
// =============================================================================
// WHY THE WHOLE FILE IS READ INTO MEMORY FIRST
// =============================================================================
// The simplest correct implementation reads the file once and maps segments out
// of that buffer. It costs one allocation the size of the executable and means
// there is no seek-and-partial-read path to get wrong.
//
// It is also a limitation worth naming: a program larger than comfortable kernel
// heap space cannot be loaded this way, and the fix is to read each segment
// directly from the file at its offset. That arrives with processes at v0.5,
// where the loader will already be reading into a per-process address space and
// the extra complexity is unavoidable rather than optional.
// =============================================================================

#include "afriyie/elf.h"
#include "afriyie/hal.h"
#include "afriyie/pmm.h"
#include "afriyie/heap.h"
#include "afriyie/log.h"
#include "afriyie/assert.h"
#include "afriyie/kstring.h"
#include "afriyie/syscall.h"

#if AF_TARGET_X86_64
#include "../arch/x86_64/x86_64.h"
#endif

// -----------------------------------------------------------------------------
// On-disk structures
// -----------------------------------------------------------------------------
#define EI_NIDENT 16

#define ELF_MAGIC_0 0x7F
#define ELF_MAGIC_1 'E'
#define ELF_MAGIC_2 'L'
#define ELF_MAGIC_3 'F'

#define ELFCLASS64 2
#define ELFDATA2LSB 1
#define ET_EXEC    2
#define ET_DYN     3
#define EM_X86_64  62

#define PT_LOAD    1

#define PF_X 1
#define PF_W 2
#define PF_R 4

typedef struct AF_PACKED {
    af_u8  e_ident[EI_NIDENT];
    af_u16 e_type;
    af_u16 e_machine;
    af_u32 e_version;
    af_u64 e_entry;
    af_u64 e_phoff;
    af_u64 e_shoff;
    af_u32 e_flags;
    af_u16 e_ehsize;
    af_u16 e_phentsize;
    af_u16 e_phnum;
    af_u16 e_shentsize;
    af_u16 e_shnum;
    af_u16 e_shstrndx;
} elf64_header_t;

AF_STATIC_ASSERT_SIZE(elf64_header_t, 64);

typedef struct AF_PACKED {
    af_u32 p_type;
    af_u32 p_flags;
    af_u64 p_offset;
    af_u64 p_vaddr;
    af_u64 p_paddr;
    af_u64 p_filesz;
    af_u64 p_memsz;
    af_u64 p_align;
} elf64_program_header_t;

AF_STATIC_ASSERT_SIZE(elf64_program_header_t, 56);

// The largest executable this loader will accept. A bound is required because
// the size comes from disk: without one, a corrupt header asks for a gigabyte
// and the allocation fails in a way that looks like memory exhaustion rather
// than a bad file.
#define AF_ELF_MAX_IMAGE (4 * AF_MIB)

// Where the user stack lives. Above the program's own image, and well clear of
// the identity map so it genuinely exercises the page-table walk.
#define AF_ELF_STACK_TOP 0x0000000100100000ULL
#define AF_ELF_STACK_SIZE (64 * AF_KIB)

// -----------------------------------------------------------------------------
// Validation
// -----------------------------------------------------------------------------
static af_status_t validate_header(const elf64_header_t *h, af_size file_size)
{
    if (file_size < sizeof(elf64_header_t)) {
        af_error("elf", "file is %u bytes, smaller than an ELF header",
                 (af_u32)file_size);
        return AF_ERR_INVAL;
    }

    if (h->e_ident[0] != ELF_MAGIC_0 || h->e_ident[1] != ELF_MAGIC_1 ||
        h->e_ident[2] != ELF_MAGIC_2 || h->e_ident[3] != ELF_MAGIC_3) {
        af_error("elf", "not an ELF file (magic is %02X %02X %02X %02X)",
                 h->e_ident[0], h->e_ident[1], h->e_ident[2], h->e_ident[3]);
        return AF_ERR_INVAL;
    }

    if (h->e_ident[4] != ELFCLASS64) {
        af_error("elf", "not a 64-bit ELF (class %u)", h->e_ident[4]);
        return AF_ERR_NOTSUP;
    }

    if (h->e_ident[5] != ELFDATA2LSB) {
        af_error("elf", "not little-endian (data encoding %u)", h->e_ident[5]);
        return AF_ERR_NOTSUP;
    }

    if (h->e_type != ET_EXEC && h->e_type != ET_DYN) {
        af_error("elf", "not an executable (type %u; 1 is relocatable)",
                 h->e_type);
        return AF_ERR_NOTSUP;
    }

    if (h->e_machine != EM_X86_64) {
        af_error("elf", "built for machine %u, not x86_64 (%u)",
                 h->e_machine, (af_u32)EM_X86_64);
        return AF_ERR_NOTSUP;
    }

    if (h->e_phnum == 0) {
        af_error("elf", "no program headers — nothing to load");
        return AF_ERR_INVAL;
    }

    if (h->e_phentsize != sizeof(elf64_program_header_t)) {
        af_error("elf", "program header size is %u, expected %u",
                 h->e_phentsize, (af_u32)sizeof(elf64_program_header_t));
        return AF_ERR_INVAL;
    }

    // The header table must lie inside the file. Without this check a corrupt
    // e_phoff walks the parser off the end of the buffer it was handed.
    af_u64 table_end = h->e_phoff + (af_u64)h->e_phnum * h->e_phentsize;
    if (h->e_phoff > file_size || table_end > file_size) {
        af_error("elf", "program header table (offset %llu, %u entries) runs "
                        "past the end of a %u-byte file",
                 (unsigned long long)h->e_phoff, h->e_phnum, (af_u32)file_size);
        return AF_ERR_INVAL;
    }

    return AF_OK;
}

// -----------------------------------------------------------------------------
// Permissions
// -----------------------------------------------------------------------------
static af_u32 segment_to_hal_flags(af_u32 p_flags)
{
    af_u32 flags = HAL_PRESENT | HAL_USER;

    if ((p_flags & PF_W) != 0) {
        flags |= HAL_WRITABLE;
    }
    if ((p_flags & PF_X) != 0) {
        flags |= HAL_EXEC;
    }

    // A segment that is neither writable nor executable is read-only, which is
    // exactly what HAL_EXEC being absent and HAL_WRITABLE being absent produces.
    // There is no separate "read" flag because read access is implied by
    // presence — the loader does not have to ask for it.
    return flags;
}

static const char *segment_flags_name(af_u32 p_flags)
{
    static char buffer[4];
    buffer[0] = (p_flags & PF_R) ? 'r' : '-';
    buffer[1] = (p_flags & PF_W) ? 'w' : '-';
    buffer[2] = (p_flags & PF_X) ? 'x' : '-';
    buffer[3] = '\0';
    return buffer;
}

// -----------------------------------------------------------------------------
// Loading
// -----------------------------------------------------------------------------
af_status_t elf_load(fat32_volume_t *vol, const char *path,
                     af_vaddr *out_entry, af_vaddr *out_stack_top)
{
    if (vol == NULL || path == NULL || out_entry == NULL) {
        return AF_ERR_INVAL;
    }

    // --- read the file --------------------------------------------------------
    fat32_entry_t entry;
    af_status_t rc = fat32_lookup(vol, path, &entry);
    if (af_status_err(rc)) {
        return rc;
    }

    if (entry.is_directory) {
        return AF_ERR_ISDIR;
    }

    if (entry.size == 0 || entry.size > AF_ELF_MAX_IMAGE) {
        af_error("elf", "'%s' is %u bytes; the loader accepts up to %u",
                 path, entry.size, (af_u32)AF_ELF_MAX_IMAGE);
        return AF_ERR_INVAL;
    }

    af_u8 *image = (af_u8 *)kmalloc(entry.size);
    if (image == NULL) {
        af_error("elf", "no memory for a %u-byte executable", entry.size);
        return AF_ERR_NOMEM;
    }

    af_u32 file_size = 0;
    rc = fat32_read_file(vol, path, image, entry.size, &file_size);
    if (af_status_err(rc)) {
        kfree(image);
        return rc;
    }

    af_info("elf", "loading '%s': %u bytes read", path, file_size);

    // --- validate -------------------------------------------------------------
    const elf64_header_t *header = (const elf64_header_t *)image;

    rc = validate_header(header, file_size);
    if (af_status_err(rc)) {
        kfree(image);
        return rc;
    }

    af_info("elf", "  entry 0x%lX, %u program header(s), %s",
            header->e_entry, header->e_phnum,
            (header->e_type == ET_EXEC) ? "ET_EXEC" : "ET_DYN");

    // Copied out of the image now, while the image is still alive.
    //
    // `header` points INTO the buffer that is kfree'd at the end of the segment
    // loop, so reading header->e_entry afterwards is a use-after-free. It does
    // not fault: the heap hands the same block straight back out, and the freed
    // memory is still mapped, so the read succeeds and returns whatever the next
    // allocation put there. On the first boot it returned 0, and the kernel
    // entered ring 3 at address 0 — a page fault in the middle of nowhere, two
    // functions away from the line that caused it.
    const af_vaddr entry_point = header->e_entry;
    const af_u32     phnum = header->e_phnum;
    const af_u64     phoff = header->e_phoff;
    const af_u16     phentsize = header->e_phentsize;

    // --- map the segments -----------------------------------------------------
    hal_pt_root_t root = hal_get_page_table();
    af_u32 loaded = 0;

    for (af_u32 i = 0; i < phnum; i++) {
        const elf64_program_header_t *ph =
            (const elf64_program_header_t *)(image + phoff +
                                             (af_u64)i * phentsize);

        if (ph->p_type != PT_LOAD) {
            continue;
        }

        if (ph->p_memsz == 0) {
            continue;   // a zero-length segment is legal and loads nothing
        }

        // The segment's file data must lie inside the file. This is the check
        // that stops a corrupt p_offset from reading past the buffer.
        if (ph->p_offset > file_size ||
            ph->p_offset + ph->p_filesz > file_size) {
            af_error("elf", "segment %u: file range [%llu, %llu) is outside the "
                            "%u-byte file",
                     i, (unsigned long long)ph->p_offset,
                     (unsigned long long)(ph->p_offset + ph->p_filesz),
                     file_size);
            kfree(image);
            return AF_ERR_INVAL;
        }

        // p_filesz must never exceed p_memsz: the extra bytes in memory are the
        // BSS, and a file claiming more file bytes than memory bytes is
        // malformed. Trusting it would write past the end of the mapping.
        if (ph->p_filesz > ph->p_memsz) {
            af_error("elf", "segment %u: p_filesz (%llu) exceeds p_memsz (%llu)",
                     i, (unsigned long long)ph->p_filesz,
                     (unsigned long long)ph->p_memsz);
            kfree(image);
            return AF_ERR_INVAL;
        }

        af_u64 start = AF_ALIGN_DOWN(ph->p_vaddr, AF_PAGE_SIZE);
        af_u64 end   = AF_ALIGN_UP(ph->p_vaddr + ph->p_memsz, AF_PAGE_SIZE);
        af_u64 pages = (end - start) / AF_PAGE_SIZE;

        af_u32 hal_flags = segment_to_hal_flags(ph->p_flags);

        if ((start & ~(af_u64)AF_USER_TOP) != 0) {
            af_error("elf", "segment %u: load address 0x%lX is not in the user "
                            "half of the address space", i, ph->p_vaddr);
            kfree(image);
            return AF_ERR_INVAL;
        }

        af_info("elf", "  segment %u: 0x%lX..0x%lX (%llu pages) %s%s%s",
                i, start, end - 1, (unsigned long long)pages,
                segment_flags_name(ph->p_flags),
                (ph->p_filesz < ph->p_memsz) ? " +bss" : "",
                (ph->p_align > AF_PAGE_SIZE) ? " aligned" : "");

        // =====================================================================
        // ONE FRAME PER PAGE, MAPPED AT A CONSECUTIVE VIRTUAL ADDRESS
        // =====================================================================
        // Not a contiguous physical run. The program's segments are laid out for
        // the virtual address space, and physical contiguity is not something
        // the loader can promise on a machine that has been running for a while
        // — requiring it would make loading fail on a fragmented system for no
        // reason. Each frame is allocated on its own and mapped where it belongs.
        // =====================================================================
        for (af_u64 page = 0; page < pages; page++) {
            af_paddr frame = pmm_alloc_frame_z();
            if (frame == AF_FRAME_INVALID) {
                af_error("elf", "segment %u: out of memory after %llu of %llu "
                                "pages", i, (unsigned long long)page,
                         (unsigned long long)pages);
                kfree(image);
                return AF_ERR_NOMEM;
            }

            af_vaddr va = (af_vaddr)(start + page * AF_PAGE_SIZE);

            rc = hal_map_page(root, va, frame, hal_flags);
            if (af_status_err(rc)) {
                af_error("elf", "segment %u: could not map 0x%lX (%s)",
                         i, va, af_status_name(rc));
                pmm_free_frame(frame);
                kfree(image);
                return rc;
            }

            // Copy the part of this page that the file provides.
            //
            // The frame is zeroed at allocation, so the BSS tail and any pages
            // past p_filesz are already zero — which is why pmm_alloc_frame_z is
            // used rather than pmm_alloc_frame. Relying on a non-zeroing
            // allocator here would leave uninitialised globals holding whatever
            // the previous owner left behind.
            af_u64 page_start = start + page * AF_PAGE_SIZE;
            af_u64 file_start = ph->p_vaddr;
            af_u64 file_end   = ph->p_vaddr + ph->p_filesz;

            if (page_start < file_end && page_start + AF_PAGE_SIZE > file_start) {
                af_u64 copy_from = (page_start > file_start) ? page_start : file_start;
                af_u64 copy_to   = (page_start + AF_PAGE_SIZE < file_end)
                                       ? page_start + AF_PAGE_SIZE : file_end;

                af_u64 offset_in_page = copy_from - page_start;
                af_u64 length = copy_to - copy_from;
                af_u64 offset_in_file = ph->p_offset + (copy_from - ph->p_vaddr);

                af_memcpy((af_u8 *)(af_uptr)frame + offset_in_page,
                          image + offset_in_file, (af_size)length);
            }
        }

        loaded++;
    }

    kfree(image);

    if (loaded == 0) {
        af_error("elf", "'%s' contains no PT_LOAD segments", path);
        return AF_ERR_INVAL;
    }

    // --- the stack ------------------------------------------------------------
    //
    // Mapped user + writable and NOT executable. NX on the stack is the single
    // cheapest thing that turns a buffer overflow from code execution into a
    // page fault, and it costs nothing to set.
    af_u64 stack_pages = AF_ELF_STACK_SIZE / AF_PAGE_SIZE;

    for (af_u64 page = 0; page < stack_pages; page++) {
        af_paddr frame = pmm_alloc_frame_z();
        if (frame == AF_FRAME_INVALID) {
            return AF_ERR_NOMEM;
        }

        af_vaddr va = (af_vaddr)(AF_ELF_STACK_TOP - AF_ELF_STACK_SIZE +
                                 page * AF_PAGE_SIZE);

        rc = hal_map_page(root, va, frame, HAL_PRESENT | HAL_WRITABLE | HAL_USER);
        if (af_status_err(rc)) {
            pmm_free_frame(frame);
            return rc;
        }
    }

    af_info("elf", "  stack: %u KiB at 0x%lX..0x%lX (user+writable, non-exec)",
            (af_u32)(AF_ELF_STACK_SIZE / AF_KIB),
            (af_u64)(AF_ELF_STACK_TOP - AF_ELF_STACK_SIZE),
            (af_u64)(AF_ELF_STACK_TOP - 1));

    *out_entry = entry_point;
    if (out_stack_top != NULL) {
        *out_stack_top = AF_ELF_STACK_TOP;
    }

    return AF_OK;
}

// -----------------------------------------------------------------------------
// Load and run
// -----------------------------------------------------------------------------
void elf_exec(fat32_volume_t *vol, const char *path)
{
    af_vaddr entry = 0;
    af_vaddr stack_top = 0;

    af_status_t rc = elf_load(vol, path, &entry, &stack_top);
    if (af_status_err(rc)) {
        af_panic("elf: could not load '%s' (%s)", path, af_status_name(rc));
    }

    af_info("elf", "entering '%s' at 0x%lX in ring 3", path, entry);
    af_info("elf", "----------------------------------------------------------");

    af_marker("AF_EXEC_PREPARED");

    // Never returns: the program's exit system call terminates this thread.
    af_x86_enter_user_mode(entry, stack_top);
}
