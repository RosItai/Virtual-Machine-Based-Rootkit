; gdt.asm
; A single flat GDT with descriptors for every mode this kernel will
; run code in: 16-bit (real-mode-compatible, e.g. a future SMP AP
; trampoline), 32-bit (protected mode - what we're in right now), and
; 64-bit (long mode, once paging + CR0/CR4/EFER are set up).
;
; Two things have to be right for a descriptor to actually behave the
; way its mode implies:
;   - L (long) and D/B (size) are mutually exclusive per the SDM: a
;     64-bit code descriptor needs L=1, D/B=0. Setting both is the
;     classic way a GDT "mostly works" but misbehaves in one mode -
;     code64 below only ever sets SEG_LONG_FLAG, never together with
;     SEG_SIZE_FLAG.
;   - Base/limit/granularity are only meaningful outside 64-bit mode:
;     the CPU ignores them entirely for 64-bit code/data (everything
;     is flat regardless of what's written here), but the 16-/32-bit
;     descriptors still need correct values since real/protected mode
;     code actually uses them.
;
; This only builds the table and a loader function - see load_gdt at
; the bottom for what "loading" it actually does (and doesn't do yet).

bits 32

%include "src/boot/macros.asm"

section .data
align 8
_gdt:
.null:
    dq 0

.code64: equ $ - _gdt
    dw 0xFFFF                                    ; limit 0-15
    dw 0                                          ; base 0-15
    db 0                                          ; base 16-23
    db SEG_A | SEG_S | SEG_P | SEG_RW | SEG_E     ; access byte
    db 0x0f | (SEG_LONG_FLAG << 4)                ; limit 16-19 | flags (L=1, D/B=0)
    db 0                                          ; base 24-31

.data64: equ $ - _gdt
    dw 0xFFFF
    dw 0
    db 0
    db SEG_A | SEG_S | SEG_P | SEG_RW
    db 0x0f
    db 0

.code32: equ $ - _gdt
    dw 0xFFFF
    dw 0
    db 0
    db SEG_A | SEG_S | SEG_P | SEG_RW | SEG_E
    db 0x0f | ((SEG_SIZE_FLAG | SEG_FLAG_G) << 4)  ; D/B=1 (32-bit), G=1 -> flat 4GB
    db 0

.data32: equ $ - _gdt
    dw 0xFFFF
    dw 0
    db 0
    db SEG_A | SEG_S | SEG_P | SEG_RW
    db 0x0f | ((SEG_SIZE_FLAG | SEG_FLAG_G) << 4)
    db 0

.code16: equ $ - _gdt
    dw 0xFFFF
    dw 0
    db 0
    db SEG_A | SEG_S | SEG_P | SEG_RW | SEG_E
    db 0x0f | (SEG_FLAG_G << 4)                    ; D/B=0 (16-bit), G=1 -> flat "unreal mode" style limit
    db 0

.data16: equ $ - _gdt
    dw 0xFFFF
    dw 0
    db 0
    db SEG_A | SEG_S | SEG_P | SEG_RW
    db 0x0f | (SEG_FLAG_G << 4)
    db 0

; TSS descriptor - 16 bytes in long mode (needs a full 64-bit base),
; unlike every descriptor above. VMX requires TR to reference a valid,
; loaded TSS for both host and guest state (see vmx.c's
; initializeVmcs) - this didn't exist before since nothing needed a
; real TSS until now. Base is left 0 here and patched at runtime by
; set_tss_base once the TSS structure's actual address is known -
; deliberately NOT computed here via compile-time address arithmetic
; split across these scattered byte fields, since that's exactly the
; kind of NASM+elf64 relocation pattern that silently broke once
; already this session (see the "call TRAMPOLINE_ADDR" bug).
TSS_SIZE equ 104   ; minimal 64-bit TSS (no IST/RSP0 use yet)

.tss: equ $ - _gdt
    dw TSS_SIZE - 1                ; limit 0-15
    dw 0                           ; base 0-15 (patched)
    db 0                           ; base 16-23 (patched)
    db SEG_A | SEG_E | SEG_P       ; access byte: type=0x9 (available 64-bit TSS), S=0 (system descriptor)
    db 0x00                        ; limit 16-19 | flags (all 0 for a TSS)
    db 0                           ; base 24-31 (patched)
    dd 0                           ; base 32-63 (patched)
    dd 0                           ; reserved, must be 0

.pointer:
    dw .pointer - _gdt - 1   ; GDT limit = size of the table above, minus 1
    dq _gdt                  ; GDT base (8 bytes, so this also works unchanged with a 64-bit lgdt later)

; Total GDT byte count, exported so SMP trampolines can build their own
; 6-byte GDTR without duplicating the hardcoded value.
GDT_BYTES equ .pointer - _gdt
global GDT_BYTES
global _gdt

; Selector numbers are just the byte offset into the table (8 bytes per
; descriptor, RPL 0), which is exactly what the equ's above computed.
GDT_CODE64_SEL equ .code64
GDT_DATA64_SEL equ .data64
GDT_CODE32_SEL equ .code32
GDT_DATA32_SEL equ .data32
GDT_CODE16_SEL equ .code16
GDT_DATA16_SEL equ .data16
GDT_TSS_SEL    equ .tss

global GDT_CODE64_SEL
global GDT_DATA64_SEL
global GDT_CODE32_SEL
global GDT_DATA32_SEL
global GDT_CODE16_SEL
global GDT_DATA16_SEL
global GDT_TSS_SEL

section .text
global load_gdt

; void load_gdt(void)
; Installs our own GDT - replacing whatever GRUB left in place. The
; Multiboot2 spec guarantees a flat 32-bit CS at boot but says nothing
; about it staying valid, so we take ownership of our own table early,
; same as with paging. Leaves us in 32-bit protected mode, just now
; running on our own descriptors; CODE64/CODE16/DATA16 sit ready for
; later use (the long-mode far jump, an SMP trampoline, etc) - this
; function only activates CODE32/DATA32.
load_gdt:
    lgdt [_gdt.pointer]
    jmp GDT_CODE32_SEL:.reload_segments
.reload_segments:
    mov ax, GDT_DATA32_SEL
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    ret

; void set_tss_base(uint64_t tssAddress) - SysV calling convention,
; so the address arrives in rdi. Patches the TSS descriptor's base
; fields directly in memory (a plain data write, not the kind of
; compile-time address arithmetic that's risky under elf64 - see the
; .tss comment above). Must run in 64-bit mode since the address is a
; genuine 64-bit value; called from vmx.c once, before the first LTR.
bits 64
global set_tss_base
set_tss_base:
    ; GDT_TSS_SEL (not the local ".tss" label) - by this point
    ; load_gdt: has already introduced a new non-local label, so a
    ; bare ".tss" reference here would resolve against the wrong
    ; scope (NASM scopes local labels to the most recent non-local
    ; label at the point of *reference*, not of definition) and fail
    ; to assemble. GDT_TSS_SEL is the same value (already resolved
    ; when it was defined, right after .tss itself), just reachable
    ; unambiguously from anywhere in the file.
    mov [_gdt + GDT_TSS_SEL + 2], di      ; base 0-15
    mov rax, rdi
    shr rax, 16
    mov [_gdt + GDT_TSS_SEL + 4], al      ; base 16-23
    mov rax, rdi
    shr rax, 24
    mov [_gdt + GDT_TSS_SEL + 7], al      ; base 24-31
    mov rax, rdi
    shr rax, 32
    mov [_gdt + GDT_TSS_SEL + 8], eax     ; base 32-63
    ret
bits 32
