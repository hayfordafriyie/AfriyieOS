// SPDX-License-Identifier: MIT
// AfriyieOS — fundamental types
//
// Freestanding C provides <stdint.h>, <stddef.h>, <stdbool.h> and <stdarg.h>
// even without a hosted libc, so we build on those and add our own fixed-width
// aliases. Nothing here may assume a hosted environment.

#ifndef AFRIYIE_TYPES_H
#define AFRIYIE_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>

// -----------------------------------------------------------------------------
// Integer aliases
//
// `af_*` names are used throughout the kernel so that a future move to a
// different base type (or to Rust bindings) is a single-file change.
// -----------------------------------------------------------------------------
typedef uint8_t   af_u8;
typedef uint16_t  af_u16;
typedef uint32_t  af_u32;
typedef uint64_t  af_u64;

typedef int8_t    af_i8;
typedef int16_t   af_i16;
typedef int32_t   af_i32;
typedef int64_t   af_i64;

typedef size_t    af_size;
typedef ptrdiff_t af_ssize;
typedef uintptr_t af_uptr;
typedef intptr_t  af_iptr;

typedef af_u64    af_paddr;   // physical address
typedef af_u64    af_vaddr;   // virtual address

// -----------------------------------------------------------------------------
// Limits
// -----------------------------------------------------------------------------
#define AF_U8_MAX   UINT8_MAX
#define AF_U16_MAX  UINT16_MAX
#define AF_U32_MAX  UINT32_MAX
#define AF_U64_MAX  UINT64_MAX

#define AF_I32_MAX  INT32_MAX
#define AF_I32_MIN  INT32_MIN

// -----------------------------------------------------------------------------
// Common constants
// -----------------------------------------------------------------------------
#define AF_KIB  (1024ULL)
#define AF_MIB  (1024ULL * AF_KIB)
#define AF_GIB  (1024ULL * AF_MIB)
#define AF_TIB  (1024ULL * AF_GIB)

#define AF_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

#define AF_UNUSED(x)     ((void)(x))
#define AF_MIN(a, b)     ((a) < (b) ? (a) : (b))
#define AF_MAX(a, b)     ((a) > (b) ? (a) : (b))
#define AF_ALIGN_UP(v, a)   (((v) + ((a) - 1)) & ~((a) - 1))
#define AF_ALIGN_DOWN(v, a) ((v) & ~((a) - 1))
#define AF_IS_ALIGNED(v, a) (((v) & ((a) - 1)) == 0)

// -----------------------------------------------------------------------------
// Compiler attributes (see also compiler.h)
// -----------------------------------------------------------------------------
#if defined(__GNUC__) || defined(__clang__)
#  define AF_PACKED        __attribute__((packed))
#  define AF_ALIGNED(n)    __attribute__((aligned(n)))
#  define AF_NORETURN      __attribute__((noreturn))
#  define AF_PRINTF(f, a)  __attribute__((format(printf, f, a)))
#  define AF_INLINE        static inline __attribute__((always_inline))
#  define AF_UNUSED_PARAM  __attribute__((unused))
#  define AF_SECTION(s)    __attribute__((section(s)))
#  define AF_WEAK          __attribute__((weak))
#  define AF_MAYBE_UNUSED  __attribute__((unused))
#else
#  define AF_PACKED
#  define AF_ALIGNED(n)
#  define AF_NORETURN
#  define AF_PRINTF(f, a)
#  define AF_INLINE        static inline
#  define AF_UNUSED_PARAM
#  define AF_SECTION(s)
#  define AF_WEAK
#  define AF_MAYBE_UNUSED
#endif

// -----------------------------------------------------------------------------
// Compile-time checks
//
// REQUIRED_* produce a hard compile error. They are used to pin down hardware
// and firmware structure layouts, which is the single most common source of
// silent triple faults during boot.
// -----------------------------------------------------------------------------
#define AF_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#define AF_STATIC_ASSERT_SIZE(type, size) \
    _Static_assert(sizeof(type) == (size), #type " has an unexpected size")
#define AF_STATIC_ASSERT_OFFSET(type, member, off) \
    _Static_assert(offsetof(type, member) == (off), \
                   #type "." #member " is at the wrong offset")

// -----------------------------------------------------------------------------
// Fixed-size integer helpers
// -----------------------------------------------------------------------------
AF_INLINE af_u64 af_u64_from_u32s(af_u32 hi, af_u32 lo)
{
    return ((af_u64)hi << 32) | (af_u64)lo;
}

AF_INLINE af_u32 af_u64_hi32(af_u64 v) { return (af_u32)(v >> 32); }
AF_INLINE af_u32 af_u64_lo32(af_u64 v) { return (af_u32)(v & 0xFFFFFFFFu); }

#endif // AFRIYIE_TYPES_H
