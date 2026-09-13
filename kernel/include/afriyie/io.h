// SPDX-License-Identifier: MIT
// AfriyieOS — memory-mapped I/O and port I/O accessors
//
// Rule (blueprint section 4.3): ALL device register access goes through these
// helpers. Direct dereferences of MMIO addresses are forbidden, because the
// compiler is free to reorder, widen or eliminate plain loads and stores.

#ifndef AFRIYIE_IO_H
#define AFRIYIE_IO_H

#include "types.h"

// -----------------------------------------------------------------------------
// Compiler barriers
// -----------------------------------------------------------------------------
#define af_compiler_barrier() __asm__ __volatile__("" ::: "memory")

// -----------------------------------------------------------------------------
// MMIO accessors — little-endian devices on both architectures
// -----------------------------------------------------------------------------
AF_INLINE af_u8 af_mmio_read8(af_uptr addr)
{
    return *(volatile af_u8 *)addr;
}

AF_INLINE af_u16 af_mmio_read16(af_uptr addr)
{
    return *(volatile af_u16 *)addr;
}

AF_INLINE af_u32 af_mmio_read32(af_uptr addr)
{
    return *(volatile af_u32 *)addr;
}

AF_INLINE af_u64 af_mmio_read64(af_uptr addr)
{
    return *(volatile af_u64 *)addr;
}

AF_INLINE void af_mmio_write8(af_uptr addr, af_u8 value)
{
    *(volatile af_u8 *)addr = value;
    af_compiler_barrier();
}

AF_INLINE void af_mmio_write16(af_uptr addr, af_u16 value)
{
    *(volatile af_u16 *)addr = value;
    af_compiler_barrier();
}

AF_INLINE void af_mmio_write32(af_uptr addr, af_u32 value)
{
    *(volatile af_u32 *)addr = value;
    af_compiler_barrier();
}

AF_INLINE void af_mmio_write64(af_uptr addr, af_u64 value)
{
    *(volatile af_u64 *)addr = value;
    af_compiler_barrier();
}

// Read-modify-write on a 32-bit register (the common "set bits" idiom).
AF_INLINE af_u32 af_mmio_set_bits32(af_uptr addr, af_u32 mask)
{
    af_u32 v = af_mmio_read32(addr);
    af_mmio_write32(addr, v | mask);
    return af_mmio_read32(addr);
}

AF_INLINE af_u32 af_mmio_clear_bits32(af_uptr addr, af_u32 mask)
{
    af_u32 v = af_mmio_read32(addr);
    af_mmio_write32(addr, v & ~mask);
    return af_mmio_read32(addr);
}

// -----------------------------------------------------------------------------
// Port I/O — x86_64 only
// -----------------------------------------------------------------------------
#if AF_TARGET_X86_64

AF_INLINE void af_outb(af_u16 port, af_u8 value)
{
    __asm__ __volatile__("outb %0, %1" :: "a"(value), "Nd"(port));
}

AF_INLINE af_u8 af_inb(af_u16 port)
{
    af_u8 v;
    __asm__ __volatile__("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

AF_INLINE void af_outw(af_u16 port, af_u16 value)
{
    __asm__ __volatile__("outw %0, %1" :: "a"(value), "Nd"(port));
}

AF_INLINE af_u16 af_inw(af_u16 port)
{
    af_u16 v;
    __asm__ __volatile__("inw %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

AF_INLINE void af_outl(af_u16 port, af_u32 value)
{
    __asm__ __volatile__("outl %0, %1" :: "a"(value), "Nd"(port));
}

AF_INLINE af_u32 af_inl(af_u16 port)
{
    af_u32 v;
    __asm__ __volatile__("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

// Small delay for slow devices (the classic port-0x80 write).
AF_INLINE void af_io_wait(void)
{
    af_outb(0x80, 0);
}

// -----------------------------------------------------------------------------
// CPU control (x86_64)
// -----------------------------------------------------------------------------
AF_INLINE void af_halt(void)          { __asm__ __volatile__("hlt"); }
AF_INLINE void af_interrupts_enable(void)  { __asm__ __volatile__("sti"); }
AF_INLINE void af_interrupts_disable(void) { __asm__ __volatile__("cli"); }

AF_INLINE af_u64 af_save_flags_and_disable_interrupts(void)
{
    af_u64 flags;
    __asm__ __volatile__("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

AF_INLINE void af_restore_flags(af_u64 flags)
{
    __asm__ __volatile__("pushq %0; popfq" :: "r"(flags) : "memory", "cc");
}

AF_INLINE bool af_interrupts_enabled(void)
{
    af_u64 flags;
    __asm__ __volatile__("pushfq; popq %0" : "=r"(flags));
    return (flags & (1ULL << 9)) != 0;
}

static inline void af_read_msr(af_u32 msr, af_u32 *lo, af_u32 *hi)
{
    __asm__ __volatile__("rdmsr" : "=a"(*lo), "=d"(*hi) : "c"(msr));
}

static inline void af_write_msr(af_u32 msr, af_u32 lo, af_u32 hi)
{
    __asm__ __volatile__("wrmsr" :: "a"(lo), "d"(hi), "c"(msr));
}

AF_INLINE void af_cpuid(af_u32 leaf, af_u32 *a, af_u32 *b, af_u32 *c, af_u32 *d)
{
    __asm__ __volatile__("cpuid"
                         : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                         : "a"(leaf), "c"(0));
}

AF_INLINE af_u64 af_read_cr2(void)
{
    af_u64 v;
    __asm__ __volatile__("mov %%cr2, %0" : "=r"(v));
    return v;
}

AF_INLINE af_u64 af_read_cr3(void)
{
    af_u64 v;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(v));
    return v;
}

AF_INLINE void af_write_cr3(af_u64 v)
{
    __asm__ __volatile__("mov %0, %%cr3" :: "r"(v) : "memory");
}

AF_INLINE void af_invlpg(af_vaddr va)
{
    __asm__ __volatile__("invlpg (%0)" :: "r"(va) : "memory");
}

AF_INLINE af_u64 af_read_tsc(void)
{
    af_u32 lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((af_u64)hi << 32) | lo;
}

#elif AF_TARGET_AARCH64

AF_INLINE void af_halt(void) { __asm__ __volatile__("wfi"); }

AF_INLINE void af_interrupts_enable(void)
{
    __asm__ __volatile__("msr daifclr, #2" ::: "memory");
}

AF_INLINE void af_interrupts_disable(void)
{
    __asm__ __volatile__("msr daifset, #2" ::: "memory");
}

AF_INLINE af_u64 af_save_flags_and_disable_interrupts(void)
{
    af_u64 flags;
    __asm__ __volatile__("mrs %0, daif" : "=r"(flags) :: "memory");
    af_interrupts_disable();
    return flags;
}

AF_INLINE void af_restore_flags(af_u64 flags)
{
    __asm__ __volatile__("msr daif, %0" :: "r"(flags) : "memory");
}

AF_INLINE bool af_interrupts_enabled(void)
{
    af_u64 flags;
    __asm__ __volatile__("mrs %0, daif" : "=r"(flags));
    return (flags & (1ULL << 7)) == 0;   // DAIF.I is bit 7
}

AF_INLINE af_u64 af_read_cntpct(void)
{
    af_u64 v;
    __asm__ __volatile__("mrs %0, cntpct_el0" : "=r"(v));
    return v;
}

AF_INLINE af_u64 af_read_cntfrq(void)
{
    af_u64 v;
    __asm__ __volatile__("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

AF_INLINE void af_tlbi_vmalle1(void)
{
    __asm__ __volatile__("tlbi vmalle1" ::: "memory");
}

AF_INLINE void af_tlbi_vae1(af_vaddr va)
{
    __asm__ __volatile__("tlbi vae1, %0" :: "r"(va >> 12) : "memory");
}

AF_INLINE void af_dsb(void) { __asm__ __volatile__("dsb sy" ::: "memory"); }
AF_INLINE void af_isb(void) { __asm__ __volatile__("isb" ::: "memory"); }

#else
#  error "Unknown architecture: no I/O accessors defined"
#endif

// -----------------------------------------------------------------------------
// Unified cache/TLB helpers
// -----------------------------------------------------------------------------
AF_INLINE void af_flush_tlb_page(af_vaddr va)
{
#if AF_TARGET_X86_64
    af_invlpg(va);
#else
    af_tlbi_vae1(va);
    af_dsb();
    af_isb();
#endif
}

AF_INLINE void af_flush_tlb_all(void)
{
#if AF_TARGET_X86_64
    af_write_cr3(af_read_cr3());
#else
    af_tlbi_vmalle1();
    af_dsb();
    af_isb();
#endif
}

#endif // AFRIYIE_IO_H
