; ap_trampoline.asm
; Real-mode AP wake-up trampoline. Copied by BSP to AP_TRAMPOLINE_ADDR
; (a page-aligned low-memory address) before the BSP sends INIT-SIPI.
; Each AP wakes up at CS:IP = (AP_TRAMPOLINE_ADDR >> 4) : 0 in real
; mode (CR0.PE=0, segments reset per Intel SDM 10.4.4).
;
; Phase-A stub: increment a shared arrival counter via `lock inc`, then
; HLT. The BSP reads that counter after SIPI to confirm its wake-up code
; reached every AP. Later phases replace the HLT loop with a jump back
; to the shared boot.asm .ap_entry so the AP climbs into long mode and
; joins ap_main.
;
; Position-independent: every absolute reference inside this copied range
; uses BLOB_OFF()/FLAT() so the code keeps working after being copied to
; its runtime address, same trick bios_call.asm uses for its own blob.

bits 16

section .text

; The slot SIPI will wake us at. Must be page-aligned (SIPI vector *
; 0x1000 is where the AP's CS base ends up) and in low memory so real
; mode can reach it. 0x8000 is well clear of VMBR's own low-memory
; structures (BIOS trampoline at 0x1000, E820 map at 0x2000, MBR at
; 0x7C00) and SeaBIOS's EBDA (0x9FC00+).
AP_TRAMPOLINE_ADDR equ 0x8000

; Shared counter the APs bump as they arrive. Below the trampoline so
; the two don't fight over the same page, and far enough from the MBR
; / stack scratch not to clash. BSP zeroes it before SIPI and reads it
; after.
AP_ARRIVAL_COUNTER equ 0x9000

%define BLOB_OFF(label) ((label) - ap_trampoline_start)
%define FLAT(label) (AP_TRAMPOLINE_ADDR + BLOB_OFF(label))

global ap_trampoline_start
global ap_trampoline_end

; C-visible constants so smp.c can see the same addresses without
; duplicating them. NASM's `global LABEL equ VALUE` export doesn't
; carry a value across to the linker, so we stash each one in a tiny
; data word that the C side reads as a uint32_t.
section .data
global ap_trampoline_addr_val
global ap_arrival_counter_val
ap_trampoline_addr_val:
    dd AP_TRAMPOLINE_ADDR
ap_arrival_counter_val:
    dd AP_ARRIVAL_COUNTER

section .text
ap_trampoline_start:
    cli
    ; DS must be set before touching any absolute address - SIPI resets
    ; the data segments to CS-relative, which is NOT what a
    ; flat-physical write like [AP_ARRIVAL_COUNTER] needs.
    xor ax, ax
    mov ds, ax

    ; `lock inc` is architecturally atomic across cores: whichever AP
    ; wins the write, the other APs still bump the same counter cleanly.
    lock inc dword [AP_ARRIVAL_COUNTER]

.halt:
    hlt
    jmp .halt

ap_trampoline_end:
