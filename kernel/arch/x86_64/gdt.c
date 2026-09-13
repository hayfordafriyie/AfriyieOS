// SPDX-License-Identifier: MIT
// AfriyieOS — x86_64 GDT (v0.1)
//
// v0.1 only enters kernel mode, so the GDT needs a null descriptor and kernel
// code/data. The user descriptors and the TSS are laid out now — with the exact
// selectors the rest of the kernel already assumes — so that v0.4 (user mode)
// only has to fill them in rather than renumber everything.

#include "x86_64.h"
#include "afriyie/assert.h"
#include "afriyie/kstring.h"

// Access byte bits
#define GDT_ACCESS_PRESENT     (1u << 7)
#define GDT_ACCESS_RING(r)     (((r) & 3u) << 5)
#define GDT_ACCESS_SEGMENT     (1u << 4)   // 1 = code/data (not a system descriptor)
#define GDT_ACCESS_EXECUTABLE  (1u << 3)
#define GDT_ACCESS_RW          (1u << 1)
#define GDT_ACCESS_ACCESSED    (1u << 0)

// Granularity byte bits
#define GDT_GRAN_4K            (1u << 7)
#define GDT_GRAN_LONG_MODE     (1u << 5)   // 1 = 64-bit code segment
#define GDT_GRAN_LIMIT_HIGH(l) (((l) >> 16) & 0x0Fu)

typedef struct AF_PACKED {
    // Seven slots, not six: the 64-bit TSS descriptor is SIXTEEN bytes and
    // occupies slots 5 and 6. A six-slot table leaves the upper half of the TSS
    // descriptor outside the limit, and `ltr` then faults with a #GP whose error
    // code names the selector rather than the descriptor that was too short.
    af_gdt_entry_t entries[7];
    af_gdt_pointer_t pointer;
} af_gdt_table_t;

// The GDT must be 8-byte aligned: the CPU reads descriptors as aligned
// quantities, and a misaligned table produces faults that look like anything
// but a GDT problem.
static af_gdt_table_t s_gdt __attribute__((aligned(16)));

static void set_entry(af_u32 index, af_u32 base, af_u32 limit, af_u8 access,
                      af_u8 flags)
{
    af_gdt_entry_t *e = &s_gdt.entries[index];

    e->limit_low   = (af_u16)(limit & 0xFFFF);
    e->base_low    = (af_u16)(base & 0xFFFF);
    e->base_middle = (af_u8)((base >> 16) & 0xFF);
    e->access      = access;
    e->granularity = (af_u8)(((limit >> 16) & 0x0F) | (flags & 0xF0));
    e->base_high   = (af_u8)((base >> 24) & 0xFF);
}

af_status_t af_x86_gdt_init(void)
{
    af_memset(&s_gdt, 0, sizeof(s_gdt));

    // 0: null descriptor. Required. Loading selector 0 must fault, and it is
    //    the value the CPU expects in unused segment registers.
    set_entry(0, 0, 0, 0, 0);

    // 1: 0x08 kernel code — ring 0, executable, readable, 64-bit, 4K granular.
    set_entry(1, 0, 0xFFFFF,
              GDT_ACCESS_PRESENT | GDT_ACCESS_RING(0) | GDT_ACCESS_SEGMENT |
              GDT_ACCESS_EXECUTABLE | GDT_ACCESS_RW,
              GDT_GRAN_4K | GDT_GRAN_LONG_MODE);

    // 2: 0x10 kernel data — ring 0, writable.
    set_entry(2, 0, 0xFFFFF,
              GDT_ACCESS_PRESENT | GDT_ACCESS_RING(0) | GDT_ACCESS_SEGMENT |
              GDT_ACCESS_RW,
              GDT_GRAN_4K | GDT_GRAN_LONG_MODE);

    // 3: 0x18 user code — ring 3. Reserved for v0.4.
    set_entry(3, 0, 0xFFFFF,
              GDT_ACCESS_PRESENT | GDT_ACCESS_RING(3) | GDT_ACCESS_SEGMENT |
              GDT_ACCESS_EXECUTABLE | GDT_ACCESS_RW,
              GDT_GRAN_4K | GDT_GRAN_LONG_MODE);

    // 4: 0x20 user data — ring 3. Reserved for v0.4.
    set_entry(4, 0, 0xFFFFF,
              GDT_ACCESS_PRESENT | GDT_ACCESS_RING(3) | GDT_ACCESS_SEGMENT |
              GDT_ACCESS_RW,
              GDT_GRAN_4K | GDT_GRAN_LONG_MODE);

    // 5: 0x28 TSS. The descriptor is written by af_x86_tss_init() in tss.c,
    //    which needs sixteen bytes and therefore slots 5 AND 6. Left as a
    //    present 64-bit system descriptor with a zero base here so the selector
    //    is at least valid before the TSS exists.
    set_entry(5, 0, 0,
              GDT_ACCESS_PRESENT | GDT_ACCESS_RING(0),
              0);
    set_entry(6, 0, 0, 0, 0);

    s_gdt.pointer.limit = (af_u16)(sizeof(s_gdt.entries) - 1);
    s_gdt.pointer.base  = (af_u64)(af_uptr)&s_gdt.entries[0];

    gdt_load(&s_gdt.pointer);

    return AF_OK;
}

// Writes a raw 16-byte system descriptor, used by the TSS layer.
//
// The TSS descriptor cannot go through set_entry(): that helper writes an
// ordinary 8-byte descriptor, and the 64-bit TSS one needs two slots with its
// base split across three fields and its upper 32 bits in a second slot. Passing
// it through the ordinary path produces a descriptor the CPU loads incorrectly.
void af_x86_gdt_write_raw(af_u32 index, af_u64 low, af_u64 high)
{
    af_u64 *table = (af_u64 *)&s_gdt.entries[0];

    if (index + 1 >= AF_ARRAY_LEN(s_gdt.entries)) {
        af_error("gdt", "raw descriptor at index %u does not fit the table",
                 index);
        return;
    }

    table[index]     = low;
    table[index + 1] = high;

    // The descriptor is in the live GDT already, so no reload is needed for the
    // CPU to see it — only the task register needs loading, which the TSS layer
    // does with `ltr`.
}
