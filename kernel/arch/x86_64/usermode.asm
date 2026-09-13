; SPDX-License-Identifier: MIT
; AfriyieOS — x86_64 transition into ring 3
;
;   af_x86_enter_user_mode(uint64_t entry, uint64_t user_stack_top)
;     rdi = user code entry point
;     rsi = top of the user stack
;
; =============================================================================
; HOW A RING TRANSITION ACTUALLY HAPPENS
; =============================================================================
; There is no "go to ring 3" instruction. The only way down is `iretq`, because
; iretq is the one instruction that pops a CS with a different privilege level
; and, when the new CS is ring 3, also pops SS and RSP.
;
; So the transition is built by hand: push the frame iretq expects, in the order
; it expects it, and execute iretq.
;
;   [rsp+32]  SS   = user data selector | 3
;   [rsp+24]  RSP  = user stack top
;   [rsp+16]  RFLAGS
;   [rsp+8]   CS   = user code selector | 3
;   [rsp+0]   RIP  = entry point
;
; THE RPL BITS ARE NOT OPTIONAL. The low two bits of CS and SS must be 3. A
; selector of 0x18 instead of 0x1B is a request to enter ring 0 with a ring-3
; descriptor, which faults; and the fault appears to be about the segment rather
; than about the two bits that were missed.
; =============================================================================

[bits 64]

section .text

global af_x86_enter_user_mode

; The user code and data selectors, with RPL 3 set.
%define USER_CS 0x1B     ; 0x18 | 3
%define USER_DS 0x23     ; 0x20 | 3
%define RFLAGS_IF (1 << 9)

af_x86_enter_user_mode:
    ; Interrupts must be OFF while the frame is being built. If one arrived
    ; half-built, the handler would run on a stack containing a partial iretq
    ; frame and its own return would land somewhere meaningless.
    cli

    ; Point the data segments at the USER data selector. If they were left at
    ; the kernel's, the first user-mode access through DS would fault — and the
    ; fault would name a data address rather than a selector, which is a
    ; misleading place to start looking.
    mov ax, USER_DS
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; Build the iretq frame. iretq pops in the order RIP, CS, RFLAGS, RSP, SS —
    ; so they must be PUSHED in the reverse of that.
    push qword USER_DS          ; SS
    push rsi                    ; RSP — the user's stack top
    push qword RFLAGS_IF        ; RFLAGS: interrupts enabled, nothing else set
    push qword USER_CS          ; CS
    push rdi                    ; RIP — the entry point

    ; Clear every general-purpose register before entering user mode.
    ;
    ; This is a security boundary, not tidiness. Whatever is in the registers at
    ; this moment is kernel data — pointers, addresses, the contents of whatever
    ; the kernel was doing a moment ago. A user process that starts with a
    ; register full of kernel addresses has been handed a map of the kernel for
    ; free. Every general register a user program may read must be zeroed.
    xor rax, rax
    xor rbx, rbx
    xor rcx, rcx
    xor rdx, rdx
    xor rsi, rsi
    xor rdi, rdi
    xor rbp, rbp
    xor r8,  r8
    xor r9,  r9
    xor r10, r10
    xor r11, r11
    xor r12, r12
    xor r13, r13
    xor r14, r14
    xor r15, r15

    ; iretq itself restores RFLAGS from the frame, and the IF bit in that frame
    ; is what re-enables interrupts — after the stack pointer is already the
    ; user's, which is the only safe order.
    iretq

section .note.GNU-stack noalloc noexec nowrite progbits
