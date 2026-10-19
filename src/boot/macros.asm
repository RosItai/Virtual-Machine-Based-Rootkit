; macros.asm
; Bit-flag building blocks for GDT segment descriptors (see ../gdt.asm).
; Reference: Intel SDM Vol 3A, 3.4.5 (access byte) and 3.4.5.1 (flags).

%ifndef __MACROS__
%define __MACROS__

; --- Access byte (bits 40-47 of the descriptor) ---
SEG_A   equ 1 << 0   ; Accessed
SEG_RW  equ 1 << 1   ; Readable (code segments) / Writable (data segments)
SEG_DC  equ 1 << 2   ; Direction (data) / Conforming (code)
SEG_E   equ 1 << 3   ; Executable: 1 = code segment, 0 = data segment
SEG_S   equ 1 << 4   ; Descriptor type: 1 = code/data, 0 = system segment
SEG_P   equ 1 << 7   ; Present

; --- Flags nibble (upper nibble of the limit/flags byte, bits 52-55) ---
; Gets shifted left by 4 before being OR'd in next to the limit's top nibble.
SEG_LONG_FLAG equ 1 << 1   ; L: 64-bit code segment
SEG_SIZE_FLAG equ 1 << 2   ; D/B: 1 = 32-bit segment, 0 = 16-bit segment
                            ; Mutually exclusive with SEG_LONG_FLAG - a
                            ; 64-bit code descriptor must have L=1, D/B=0.
SEG_FLAG_G    equ 1 << 3   ; G: granularity, 1 = limit counted in 4KB units

%endif
