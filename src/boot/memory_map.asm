; memory_map.asm
; Shared layout constants for the BIOS E820 memory map, fixed physical
; addresses agreed on by both the real-mode collector (bios_call.asm)
; and the long-mode reader (long_mode.asm) - %include-only, like
; macros.asm, so both sides can never disagree about where things live.
;
; Chosen range (0x2000-0x260C) sits well below the real-mode stack
; (REAL_MODE_STACK=0x7C00 in bios_call.asm, growing down) and well
; above the relocated BIOS trampoline blob (TRAMPOLINE_ADDR=0x1000,
; ~0x150 bytes) - both comfortably inside the low-memory range real
; bootloaders trust BIOS calls not to use as their own scratch space
; (see the TRAMPOLINE_ADDR comment in bios_call.asm for why that
; distinction mattered here: the original 0x8000/0x9000 placement,
; while nominally "free" memory, got silently clobbered mid-call).
%ifndef __MEMORY_MAP__
%define __MEMORY_MAP__

MEMORY_MAP_COUNT_ADDR   equ 0x2000  ; dword: number of entries actually filled in
MEMORY_MAP_ENTRIES_ADDR equ 0x2004  ; MEMORY_MAP_MAX_ENTRIES back-to-back entries
; 20 bytes: base(8) + length(8) + type(4), not the 24-byte ACPI 3.0
; "extended" format (+ extendedAttributes(4)). Reverted back to this
; after the 24-byte format reintroduced a hang/reset in this exact
; environment even with the real-mode IDT reload in place - so despite
; earlier suspecting the IDT was the *whole* story, this format choice
; independently matters too. include/memory_map.h's MemoryMapEntry
; must match (no extendedAttributes field).
MEMORY_MAP_ENTRY_SIZE   equ 20
MEMORY_MAP_MAX_ENTRIES  equ 64      ; buffer holds 64 * 20 = 1280 bytes, ending at 0x2504

%endif
