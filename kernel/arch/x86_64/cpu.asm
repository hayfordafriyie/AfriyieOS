; SPDX-License-Identifier: MIT
; AfriyieOS — x86_64 low-level CPU helpers that must be written in assembly
;
; Kept deliberately small. Anything that can be an inline function in io.h is
; an inline function in io.h; only operations that need an exact instruction
; sequence live here.

[bits 64]

section .text

; -----------------------------------------------------------------------------
; void gdt_load(const af_gdt_pointer_t *gdtr)
;
; Loads the GDT with lgdt, then reloads every segment register. This cannot be
; done from C: changing cs requires a far control transfer, and the compiler
; will not emit one for us.
; -----------------------------------------------------------------------------
global gdt_load
gdt_load:
    lgdt [rdi]

    ; Reload the data segments with the kernel data selector (index 2 => 0x10).
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    xor ax, ax
    mov fs, ax
    mov gs, ax

    ; Reload cs (index 1 => 0x08) with a far return.
    ;
    ; `o64 retf` is NASM's spelling of a far return with a 64-bit operand size
    ; (REX.W + CB). Plain `retf` would pop 32-bit operands and destroy the
    ; return address.
    ;
    ; The target address is loaded into rax with `lea`: there is no push imm64
    ; encoding, so pushing the label directly would be assembled as a
    ; sign-extended 32-bit immediate and silently truncate.
    lea rax, [rel .reload_done]
    push qword 0x08             ; target cs
    push rax                    ; target rip
    o64 retf

.reload_done:
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
