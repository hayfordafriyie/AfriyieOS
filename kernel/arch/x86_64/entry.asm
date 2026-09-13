; SPDX-License-Identifier: MIT
; AfriyieOS — x86_64 kernel entry point
;
; Reached from the boot bridge (boot/uefi) with:
;   rdi = physical address of af_boot_info
;   interrupts disabled
;   no valid stack worth using (the firmware stack is about to be reclaimed
;   by ExitBootServices)
;
; Responsibilities, in order:
;   1. disable interrupts (belt and braces; ExitBootServices does not do this)
;   2. install our own stack
;   3. zero .bss, because firmware leaves it full of garbage
;   4. call kmain — which never returns

[bits 64]

section .text
global kernel_entry
extern kmain

kernel_entry:
    cli                         ; no interrupts until the IDT exists

    ; Our own stack. kernel_stack_top is defined in .bss by the linker script.
    ; Note the `lea`: `mov rsp, kernel_stack_top` would LOAD the 8 bytes stored
    ; at that address (which is zero, since it is in .bss) rather than the
    ; address itself — the classic first-boot triple fault.
    lea rsp, [rel kernel_stack_top]

    ; Preserve the boot_info pointer across the .bss wipe. r12 is callee-saved,
    ; so C code is not allowed to clobber it.
    mov r12, rdi

    ; -------------------------------------------------------------------------
    ; Zero .bss
    ;
    ; The linker script guarantees __bss_start and __bss_end are 8-byte aligned,
    ; and the symbols mark boundaries of the whole section. This must happen
    ; BEFORE any C code runs, or every uninitialised global holds firmware
    ; garbage — which produces bugs that look like memory corruption.
    ;
    ; Note: this also clears the region that kernel_stack_top points into. That
    ; is safe because the loop uses only registers and rep stosb walks upward
    ; from __bss_start, away from the stack we are about to use.
    ; -------------------------------------------------------------------------
    mov rdi, __bss_start
    mov rcx, __bss_end
    sub rcx, rdi
    xor eax, eax
    rep stosb

    ; Establish a clean frame for the debugger's stack walker.
    xor rbp, rbp

    ; Restore the argument and enter C.
    mov rdi, r12
    call kmain

    ; kmain is declared noreturn, so reaching here means something went very
    ; wrong. Park the CPU rather than executing whatever follows in the image.
.halt:
    cli
    hlt
    jmp .halt

section .bss
align 16
global kernel_stack_bottom
global kernel_stack_top
kernel_stack_bottom:
    resb 16384                  ; 16 KiB. v0.2 replaces this with per-thread stacks.
kernel_stack_top:

section .note.GNU-stack noalloc noexec nowrite progbits
