// SPDX-License-Identifier: MIT
// AfriyieOS — bounded kernel string and memory helpers
//
// There is no libc. Every function here is bounded by construction: the
// unbounded variants (strcpy, strcat, sprintf) deliberately do not exist, so
// they cannot be used by accident (blueprint section 15).

#ifndef AFRIYIE_KSTRING_H
#define AFRIYIE_KSTRING_H

#include "types.h"

// -----------------------------------------------------------------------------
// Memory
// -----------------------------------------------------------------------------
void  *af_memset(void *dst, int value, af_size n);
void  *af_memcpy(void *dst, const void *src, af_size n);
void  *af_memmove(void *dst, const void *src, af_size n);
int    af_memcmp(const void *a, const void *b, af_size n);

// -----------------------------------------------------------------------------
// The names the COMPILER asks for, as opposed to the ones the kernel calls.
//
// GCC emits calls to these for aggregate copies, structure assignment and array
// initialisation, whatever -ffreestanding and -fno-builtin say — those flags
// describe what the program may call, not what the compiler may emit. Until v0.5
// every structure happened to be small enough to be copied inline, so nothing
// needed these; raising AF_MAX_MEMORY_REGIONS made one of them big enough, and
// the link failed with "undefined reference to `memcpy'" pointing at a file with
// no memcpy in it.
//
// Declared here so the definitions have a prototype, and so the next person to
// add a large structure does not have to rediscover why they exist.
//
// These are NOT `extern "C"`-guarded freestanding replacements for libc: they
// are what the compiler generates a call to, and the definitions forward to the
// af_* versions so there remains exactly one implementation of each.
// -----------------------------------------------------------------------------
void  *memcpy(void *dst, const void *src, af_size n);
void  *memset(void *dst, int value, af_size n);
void  *memmove(void *dst, const void *src, af_size n);
int    memcmp(const void *a, const void *b, af_size n);

// -----------------------------------------------------------------------------
// Strings
// -----------------------------------------------------------------------------
af_size af_strlen(const char *s);
int     af_strcmp(const char *a, const char *b);
int     af_strncmp(const char *a, const char *b, af_size n);
char   *af_strchr(const char *s, int c);

// Copies at most size-1 characters and always NUL-terminates.
// Returns the number of characters copied (excluding the NUL).
af_size af_strlcpy(char *dst, const char *src, af_size size);

// Appends, bounded. Returns the new length of dst (excluding the NUL).
af_size af_strlcat(char *dst, const char *src, af_size size);

// Duplicates into a caller-provided buffer with the same bounded semantics.
af_size af_strdup_into(char *dst, af_size size, const char *src);

bool af_str_has_prefix(const char *s, const char *prefix);

// -----------------------------------------------------------------------------
// Number formatting (base 10 and 16 — everything the kernel needs to log)
// -----------------------------------------------------------------------------
void af_utoa_base(af_u64 value, char *out, af_size out_size, af_u32 base,
                  bool uppercase);
void af_itoa(af_i64 value, char *out, af_size out_size);

// Returns the number of characters that would have been written, so callers
// can detect truncation (snprintf semantics).
int af_format_u64(af_u64 value, char *out, af_size out_size);
int af_format_hex(af_u64 value, bool prefix, bool zero_pad_16, char *out,
                  af_size out_size);
int af_format_bytes_human(af_u64 bytes, char *out, af_size out_size);

// -----------------------------------------------------------------------------
// Sanity
// -----------------------------------------------------------------------------
AF_INLINE bool af_is_power_of_two(af_u64 v)
{
    return v != 0 && (v & (v - 1)) == 0;
}

AF_INLINE af_u64 af_next_power_of_two(af_u64 v)
{
    if (v <= 1) {
        return 1;
    }
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v |= v >> 32;
    return v + 1;
}

#endif // AFRIYIE_KSTRING_H
