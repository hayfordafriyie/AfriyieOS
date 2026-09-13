; SPDX-License-Identifier: MIT
; AfriyieOS — x86_64 interrupt and exception entry stubs
;
; The CPU pushes a different amount of state depending on the vector, and
; whether it pushes an error code at all. These stubs normalise that into one
; uniform frame so the C dispatcher has a single structure to read.
;
; Normalised stack layout, ascending from rsp at the point isr_common runs:
;
;   r15 r14 r13 r12 r11 r10 r9 r8      <- pushed by isr_common
;   rbp rdi rsi rdx rcx rbx rax        <- pushed by isr_common
;   vector                             <- pushed by the stub
;   error code                         <- CPU, or a dummy 0 from the stub
;   rip                                <- CPU
;   cs                                 <- CPU
;   rflags                             <- CPU
;   rsp                                <- CPU (only on a privilege change)
;   ss                                 <- CPU (only on a privilege change)
;
; This layout must stay in lockstep with isr_frame_t in idt.h.

[bits 64]

section .text

extern isr_dispatch

; -----------------------------------------------------------------------------
; Stubs
; -----------------------------------------------------------------------------

; Vectors that do NOT push an error code: push a dummy so the frame is uniform.
%macro AF_ISR_NOERR 1
global isr%1
isr%1:
    push qword 0                ; dummy error code
    push qword %1               ; vector number
    jmp isr_common
%endmacro

; Vectors that DO push an error code: the CPU already put it on the stack.
%macro AF_ISR_ERR 1
global isr%1
isr%1:
    push qword %1               ; vector number (error code is already below)
    jmp isr_common
%endmacro

; --- CPU exceptions 0..31 ---
AF_ISR_NOERR 0     ; #DE divide error
AF_ISR_NOERR 1     ; #DB debug
AF_ISR_NOERR 2     ; NMI
AF_ISR_NOERR 3     ; #BP breakpoint
AF_ISR_NOERR 4     ; #OF overflow
AF_ISR_NOERR 5     ; #BR bound range
AF_ISR_NOERR 6     ; #UD invalid opcode
AF_ISR_NOERR 7     ; #NM device not available
AF_ISR_ERR   8     ; #DF double fault            (has error code)
AF_ISR_NOERR 9     ; coprocessor segment overrun
AF_ISR_ERR   10    ; #TS invalid TSS             (has error code)
AF_ISR_ERR   11    ; #NP segment not present     (has error code)
AF_ISR_ERR   12    ; #SS stack fault             (has error code)
AF_ISR_ERR   13    ; #GP general protection      (has error code)
AF_ISR_ERR   14    ; #PF page fault              (has error code)
AF_ISR_NOERR 15    ; reserved
AF_ISR_NOERR 16    ; #MF x87 floating point
AF_ISR_ERR   17    ; #AC alignment check         (has error code)
AF_ISR_NOERR 18    ; #MC machine check
AF_ISR_NOERR 19    ; #XM SIMD floating point
AF_ISR_NOERR 20    ; #VE virtualisation
AF_ISR_ERR   21    ; #CP control protection      (has error code)
AF_ISR_NOERR 22
AF_ISR_NOERR 23
AF_ISR_NOERR 24
AF_ISR_NOERR 25
AF_ISR_NOERR 26
AF_ISR_NOERR 27
AF_ISR_NOERR 28
AF_ISR_ERR   29    ; VMM communication exception (has error code)
AF_ISR_ERR   30    ; #SX security exception      (has error code)
AF_ISR_NOERR 31    ; reserved

; --- Remapped hardware IRQs 32..47 (PIC) and the rest of the table 48..255 ---
%assign v 32
%rep 224
    AF_ISR_NOERR v
%assign v v+1
%endrep

; -----------------------------------------------------------------------------
; Common entry
; -----------------------------------------------------------------------------
isr_common:
    ; Save every general-purpose register. The kernel's own code is compiled
    ; without red-zone use (-mno-red-zone), so nothing below rsp is live.
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; First argument: pointer to the normalised frame.
    mov rdi, rsp

    ; 16-byte stack alignment is mandatory before every call into C: the SysV
    ; ABI requires it and SSE-using compiled code depends on it. At this point
    ; rsp is 8-mod-16 (15 registers pushed from an already-16-aligned rsp would
    ; be aligned; the two stub pushes make it odd), so align defensively.
    mov rbp, rsp
    and rsp, -16

    call isr_dispatch

    mov rsp, rbp

    ; Restore in reverse order.
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    ; Drop the vector number and the error code.
    add rsp, 16

    iretq

section .note.GNU-stack noalloc noexec nowrite progbits
