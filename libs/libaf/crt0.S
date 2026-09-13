/* SPDX-License-Identifier: MIT
 * AfriyieOS — crt0: user program entry point
 *
 * The kernel enters here with a stack already set up and nothing else: no
 * arguments, no environment, no runtime. This is the whole of the C runtime.
 */

.section .text
.global _start
.extern main
.extern af_exit

_start:
    /* A zero frame pointer terminates a stack walk at the outermost frame, so a
     * debugger or a backtrace stops here rather than following garbage. */
    xor %rbp, %rbp

    /* The System V ABI requires the stack to be 16-byte aligned at the point of
     * a call. The kernel hands us a 16-byte aligned stack top and this is the
     * first call, so it is already correct — but relying on that silently is how
     * it stops being correct later. */
    call main

    /* main returned. A program that falls off the end of main must still
     * terminate; returning its value to the kernel is all that is required.
     * movslq, not mov: main returns int, and the upper half of RAX is garbage. */
    movslq %eax, %rdi
    call af_exit

.halt:
    hlt
    jmp .halt

/* Without this the linker assumes an executable stack for the whole program —
 * a real hardening loss, not a cosmetic warning.
 *
 * Note the flag syntax: GNU as takes the section flags as a quoted string and
 * the type after an @. The word form (noalloc/noexec/nowrite) is LLVM's
 * integrated assembler, not GAS, and GAS rejects it with
 * "junk at end of line, first unrecognized character is `n'". */
.section .note.GNU-stack,"",@progbits
