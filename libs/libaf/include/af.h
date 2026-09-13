/* SPDX-License-Identifier: MIT
 * AfriyieOS — libaf: the user-space runtime
 *
 * The only way a user program talks to the kernel. Every call is a thin wrapper
 * around the system call vector, so the register convention is written down in
 * exactly one place — here.
 *
 * This is a header plus a very small library, not a libc. There is no
 * allocator, no printf, no file I/O and no start-up code beyond the one in
 * crt0.S. Those arrive with the servers that need them.
 */

#ifndef AF_USER_H
#define AF_USER_H

typedef unsigned long  af_u64;
typedef unsigned int   af_u32;
typedef unsigned short af_u16;
typedef unsigned char  af_u8;
typedef long           af_i64;
typedef unsigned long  af_size;

#define AF_NORETURN __attribute__((noreturn))
#define AF_UNUSED   __attribute__((unused))

/* --- system call numbers, matching docs/abi/syscalls.md -------------------
 * Append-only. A retired number stays reserved forever so that a stale binary
 * receives "not supported" rather than reaching a different call.
 * ------------------------------------------------------------------------ */
#define AF_SYS_DEBUG_WRITE  0
#define AF_SYS_EXIT         1
#define AF_SYS_YIELD        3
#define AF_SYS_CLOCK_GET    5

/* ---------------------------------------------------------------------------
 * The system call stub.
 *
 * int 0x80 rather than SYSCALL: slower, but it goes through the interrupt path
 * the kernel already has written and tested. The register convention is
 * identical, so upgrading the instruction later changes this file and nothing
 * that calls it.
 *
 * RAX is both the number going in and the result coming out, which is why it is
 * a read-write operand. The "memory" clobber is required because the kernel may
 * have written to a buffer this call was given.
 * ------------------------------------------------------------------------- */
static inline af_i64 af_syscall3(af_u64 number, af_u64 a1, af_u64 a2, af_u64 a3)
{
    af_i64 result;
    __asm__ __volatile__("int $0x80"
                         : "=a"(result)
                         : "a"(number), "D"(a1), "S"(a2), "d"(a3)
                         : "memory", "rcx", "r11");
    return result;
}

static inline af_i64 af_syscall1(af_u64 number, af_u64 a1)
{
    return af_syscall3(number, a1, 0, 0);
}

/* --- the calls a program may make ---------------------------------------- */

static inline af_i64 af_write(const char *text, af_size length)
{
    return af_syscall3(AF_SYS_DEBUG_WRITE, (af_u64)text, (af_u64)length, 0);
}

static inline af_i64 af_clock_ns(void)
{
    return af_syscall1(AF_SYS_CLOCK_GET, 0);
}

static inline void af_yield(void)
{
    af_syscall1(AF_SYS_YIELD, 0);
}

/* Defined in af.c with external linkage: crt0.S calls it directly from
 * assembly, so it cannot be an inline function the compiler is free not to
 * emit. A program that ignores the return of this and keeps running is
 * executing past the point where the kernel has stopped expecting it. */
AF_NORETURN void af_exit(int code);

/* --- string helpers -------------------------------------------------------
 * There is no libc. These are what the programs in this tree actually need,
 * and nothing more gets added until something needs it.
 * ------------------------------------------------------------------------ */

static inline af_size af_strlen(const char *s)
{
    af_size n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

static inline void af_puts(const char *s)
{
    af_write(s, af_strlen(s));
}

static inline void af_putc(char c)
{
    af_write(&c, 1);
}

/* Prints an unsigned value in the given base. The buffer is fixed and the loop
 * is bounded by its size, so this cannot overrun anything whatever it is given. */
static inline void af_putu(af_u64 value, af_u32 base)
{
    char buffer[24];
    af_u32 n = 0;

    if (value == 0) {
        af_putc('0');
        return;
    }

    while (value != 0 && n < (af_u32)sizeof(buffer)) {
        af_u32 digit = (af_u32)(value % base);
        buffer[n++] = (digit < 10) ? (char)('0' + digit)
                                   : (char)('a' + digit - 10);
        value /= base;
    }

    while (n > 0) {
        af_putc(buffer[--n]);
    }
}

static inline void af_puti(af_i64 value)
{
    if (value < 0) {
        af_putc('-');
        af_putu((af_u64)(-value), 10);
        return;
    }
    af_putu((af_u64)value, 10);
}

#endif /* AF_USER_H */
