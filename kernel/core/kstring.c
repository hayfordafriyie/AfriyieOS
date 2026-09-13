// SPDX-License-Identifier: MIT
// AfriyieOS — bounded string, memory and number formatting helpers
//
// Deliberately absent: strcpy, strcat, sprintf, gets. The unbounded forms do
// not exist in this kernel, so they cannot be reached by accident.

#include "afriyie/kstring.h"
#include "afriyie/log.h"   // af_snprintf, used by af_format_bytes_human

// =============================================================================
// Memory
// =============================================================================

void *af_memset(void *dst, int value, af_size n)
{
    af_u8 *d = (af_u8 *)dst;
    af_u8  v = (af_u8)value;

    // Word-at-a-time for the bulk of the range, byte-wise for the tail. This
    // matters: memset is on the boot path and in every frame-allocation loop.
    af_u64 pattern = ((af_u64)v << 56) | ((af_u64)v << 48) |
                     ((af_u64)v << 40) | ((af_u64)v << 32) |
                     ((af_u64)v << 24) | ((af_u64)v << 16) |
                     ((af_u64)v << 8)  | (af_u64)v;

    while (n >= 8 && !AF_IS_ALIGNED((af_uptr)d, 8)) {
        *d++ = v;
        n--;
    }
    while (n >= 8) {
        *(af_u64 *)(void *)d = pattern;
        d += 8;
        n -= 8;
    }
    while (n > 0) {
        *d++ = v;
        n--;
    }
    return dst;
}

void *af_memcpy(void *dst, const void *src, af_size n)
{
    af_u8       *d = (af_u8 *)dst;
    const af_u8 *s = (const af_u8 *)src;

    while (n > 0) {
        *d++ = *s++;
        n--;
    }
    return dst;
}

void *af_memmove(void *dst, const void *src, af_size n)
{
    af_u8       *d = (af_u8 *)dst;
    const af_u8 *s = (const af_u8 *)src;

    if (d == s || n == 0) {
        return dst;
    }

    // Overlapping and dst is above src: copy backwards or we corrupt the source.
    if (d < s) {
        while (n > 0) {
            *d++ = *s++;
            n--;
        }
    } else {
        d += n;
        s += n;
        while (n > 0) {
            *--d = *--s;
            n--;
        }
    }
    return dst;
}

int af_memcmp(const void *a, const void *b, af_size n)
{
    const af_u8 *pa = (const af_u8 *)a;
    const af_u8 *pb = (const af_u8 *)b;

    for (af_size i = 0; i < n; i++) {
        if (pa[i] != pb[i]) {
            return (int)pa[i] - (int)pb[i];
        }
    }
    return 0;
}

// =============================================================================
// Strings
// =============================================================================

af_size af_strlen(const char *s)
{
    af_size n = 0;
    if (s == NULL) {
        return 0;
    }
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

int af_strcmp(const char *a, const char *b)
{
    if (a == b) {
        return 0;
    }
    if (a == NULL) {
        return -1;
    }
    if (b == NULL) {
        return 1;
    }
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return (int)(af_u8)*a - (int)(af_u8)*b;
}

int af_strncmp(const char *a, const char *b, af_size n)
{
    if (a == b || n == 0) {
        return 0;
    }
    if (a == NULL) {
        return -1;
    }
    if (b == NULL) {
        return 1;
    }
    for (af_size i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            return (int)(af_u8)a[i] - (int)(af_u8)b[i];
        }
        if (a[i] == '\0') {
            return 0;
        }
    }
    return 0;
}

char *af_strchr(const char *s, int c)
{
    if (s == NULL) {
        return NULL;
    }
    char target = (char)c;
    while (*s != '\0') {
        if (*s == target) {
            return (char *)(af_uptr)s;
        }
        s++;
    }
    return (target == '\0') ? (char *)(af_uptr)s : NULL;
}

af_size af_strlcpy(char *dst, const char *src, af_size size)
{
    af_size i = 0;

    if (dst == NULL || size == 0) {
        return 0;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return 0;
    }

    while (i + 1 < size && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
    return i;
}

af_size af_strlcat(char *dst, const char *src, af_size size)
{
    af_size dlen;

    if (dst == NULL || size == 0) {
        return 0;
    }

    dlen = af_strlen(dst);
    if (dlen >= size) {
        return size - 1;
    }
    return dlen + af_strlcpy(dst + dlen, src, size - dlen);
}

af_size af_strdup_into(char *dst, af_size size, const char *src)
{
    return af_strlcpy(dst, src, size);
}

bool af_str_has_prefix(const char *s, const char *prefix)
{
    if (s == NULL || prefix == NULL) {
        return false;
    }
    while (*prefix != '\0') {
        if (*s != *prefix) {
            return false;
        }
        s++;
        prefix++;
    }
    return true;
}

// =============================================================================
// Numbers
// =============================================================================

void af_utoa_base(af_u64 value, char *out, af_size out_size, af_u32 base,
                  bool uppercase)
{
    const char *digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    char   tmp[24];
    af_size n = 0;

    if (out == NULL || out_size == 0) {
        return;
    }
    if (base < 2 || base > 16) {
        out[0] = '\0';
        return;
    }

    if (value == 0) {
        tmp[n++] = '0';
    } else {
        while (value != 0 && n < sizeof(tmp)) {
            tmp[n++] = digits[value % base];
            value /= base;
        }
    }

    af_size i = 0;
    while (n > 0 && i + 1 < out_size) {
        out[i++] = tmp[--n];
    }
    out[i] = '\0';
}

void af_itoa(af_i64 value, char *out, af_size out_size)
{
    if (out == NULL || out_size == 0) {
        return;
    }

    if (value < 0) {
        // Negate through unsigned so INT64_MIN does not overflow.
        af_u64 magnitude = (~(af_u64)value) + 1u;
        out[0] = '-';
        af_utoa_base(magnitude, out + 1, out_size - 1, 10, false);
        return;
    }
    af_utoa_base((af_u64)value, out, out_size, 10, false);
}

int af_format_u64(af_u64 value, char *out, af_size out_size)
{
    char tmp[24];
    af_utoa_base(value, tmp, sizeof(tmp), 10, false);
    return (int)af_strlcpy(out, tmp, out_size);
}

int af_format_hex(af_u64 value, bool prefix, bool zero_pad_16, char *out,
                  af_size out_size)
{
    char   digits[24];
    af_size n = 0;

    if (out == NULL || out_size == 0) {
        return 0;
    }

    af_utoa_base(value, digits, sizeof(digits), 16, false);
    af_size dlen = af_strlen(digits);

    char buf[24];
    char *p = buf;

    if (prefix) {
        *p++ = '0';
        *p++ = 'x';
    }
    if (zero_pad_16) {
        for (af_size i = dlen; i < 16; i++) {
            *p++ = '0';
        }
    }
    for (af_size i = 0; i < dlen; i++) {
        *p++ = digits[i];
    }
    *p = '\0';

    n = (af_size)(p - buf);
    AF_UNUSED(n);
    return (int)af_strlcpy(out, buf, out_size);
}

int af_format_bytes_human(af_u64 bytes, char *out, af_size out_size)
{
    static const char *const units[] = { "B", "KiB", "MiB", "GiB", "TiB" };
    af_size unit = 0;
    af_u64  whole = bytes;
    af_u64  frac  = 0;
    bool    have_frac = false;

    while (whole >= 1024 && unit + 1 < AF_ARRAY_LEN(units)) {
        frac  = (whole % 1024) * 100 / 1024;
        whole = whole / 1024;
        have_frac = true;
        unit++;
    }

    if (!have_frac || frac == 0 || whole >= 100) {
        return af_snprintf(out, out_size, "%u %s", (af_u32)whole, units[unit]);
    }
    return af_snprintf(out, out_size, "%u.%02u %s", (af_u32)whole,
                       (af_u32)frac, units[unit]);
}

// -----------------------------------------------------------------------------
// THE NAMES THE COMPILER ASKS FOR
//
// GCC emits calls to memcpy, memset, memmove and memcmp for things the C
// language implies but does not spell out: copying a large structure, assigning
// one, initialising an array. -ffreestanding and -fno-builtin do not stop this —
// they say the standard library may not exist, which is a statement about what
// the PROGRAM calls, not about what the COMPILER generates.
//
// The kernel has af_memcpy and friends, and for four milestones that was enough
// because every structure happened to be small enough for GCC to inline the
// copy. Then AF_MAX_MEMORY_REGIONS went from 128 to 512 — boot_info grew by
// 9 KiB — and a struct copy in the self test crossed the threshold where GCC
// stops emitting moves and starts emitting a call:
//
//     undefined reference to memcpy'
//     kernel/core/selftest.c:275
//
// Note what that means: the failure had nothing to do with the line that broke,
// and nothing to do with memory maps. It is the general form of the problem —
// ANY future change that makes an aggregate big enough will do the same thing,
// at a link stage where the message points at whichever innocent file contains
// the copy.
//
// Providing the four functions is the fix, and it is the standard one for a
// freestanding target. They are thin wrappers rather than reimplementations, so
// there is still exactly one implementation of each.
//
// int memcmp(...) rather than af_i32, and size_t-compatible parameters, because
// these are the declarations GCC assumes; a mismatched signature is a subtly
// different function as far as the compiler is concerned.
// -----------------------------------------------------------------------------
void *memcpy(void *dst, const void *src, af_size n)
{
    return af_memcpy(dst, src, n);
}

void *memset(void *dst, int value, af_size n)
{
    return af_memset(dst, value, n);
}

void *memmove(void *dst, const void *src, af_size n)
{
    return af_memmove(dst, src, n);
}

int memcmp(const void *a, const void *b, af_size n)
{
    return af_memcmp(a, b, n);
}
