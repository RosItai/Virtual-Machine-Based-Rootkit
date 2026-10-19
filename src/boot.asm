; boot.asm
; Contains two things GRUB needs before it will treat this binary as a
; loadable kernel:
;   1. The Multiboot2 header (a data structure GRUB scans for)
;   2. The real entry point (_start) that GRUB jumps to once it has
;      loaded the kernel into memory and switched to 32-bit protected mode

bits 32

; ---------------------------------------------------------------------
; 1. Multiboot2 header
;    Must live within the first 32KB of the final binary and be 8-byte
;    aligned. GRUB scans for the magic number to confirm this file is
;    Multiboot2-compliant before it will boot it.
; ---------------------------------------------------------------------
section .multiboot_header
align 8
header_start:
    dd 0xe85250d6                ; magic number (identifies Multiboot2)
    dd 0                         ; architecture: 0 = 32-bit protected mode (i386)
    dd header_end - header_start ; total header length in bytes
    ; checksum: magic + architecture + length + checksum must sum to 0 (mod 2^32)
    dd -(0xe85250d6 + 0 + (header_end - header_start))

    ; required end tag (type 0, flags 0, size 8) - marks end of tag list
    dw 0
    dw 0
    dd 8
header_end:

; ---------------------------------------------------------------------
; 2. Entry point
;    GRUB hands off control here in 32-bit protected mode with:
;      EAX = 0x36d76289 (confirms we were loaded by a Multiboot2 loader)
;      EBX = pointer to the Multiboot2 boot information structure
;    Interrupts are off, paging is off, and there is no valid stack yet -
;    we have to set one up before calling any C code.
;
;    kernel_main is real 64-bit C (it needs 64-bit pointers/QWORDs for
;    the memory allocator), so it can't be called from here - the CPU
;    is still in 32-bit protected mode at this point. It's called from
;    long_mode.asm's long_mode_entry instead, once the switch is done.
;    The multiboot magic/boot info values aren't threaded through that
;    switch (kernel_main doesn't currently use them - it gets its own
;    view of memory from the BIOS E820 call instead).
; ---------------------------------------------------------------------
section .text
global _start
extern setup_paging
extern load_gdt
extern enter_long_mode

_start:
    mov esp, stack_top   ; set up a stack (stacks grow downward, so esp
                          ; starts at the top of the region we reserved)

    call load_gdt          ; take ownership of the GDT (see gdt.asm)
    call setup_paging      ; build PML4/PDPT/PD tables (see paging.asm) -
                           ; doesn't touch CR0/CR3/CR4/EFER yet

    call enter_long_mode    ; flips on PAE/CR3/LME/PG and far-jumps into
                            ; 64-bit code (long_mode.asm) - never returns

.hang:                    ; should never be reached, but just in case
    cli
    hlt
    jmp .hang

section .bss
align 16
stack_bottom:
    resb 16384            ; 16KB stack
stack_top:
