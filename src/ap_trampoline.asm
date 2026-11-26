; ap_trampoline.asm
; Real-mode AP wake-up trampoline. Copied by BSP to AP_TRAMPOLINE_ADDR
; (a page-aligned low-memory slot) before the BSP sends INIT-SIPI.
; Each AP wakes up at CS:IP = (AP_TRAMPOLINE_ADDR >> 4) : 0 in real
; mode (CR0.PE=0, segments reset per Intel SDM 10.4.4).
;
; What the blob does, in order:
;   1. Set DS/ES/SS=0 and SP to a known-safe slot - SIPI leaves SP
;      undefined, and the pushes/far-return below would otherwise
;      scribble over random low memory.
;   2. LGDT a blob-local 3-entry GDT (null/code32/data32). We can't
;      LGDT the kernel GDT at 256 MB from here because KVM doesn't
;      honour port-0x92 A20 reliably, and address line 20 is masked on
;      an AP straight out of SIPI - a read from 0x10xxxxxx would
;      wrap around bit 20 and load a garbage GDTR.
;   3. CR0.PE=1, far jump to a 32-bit landing pad inside this same
;      blob (pm_landing below). Using an in-blob target keeps every
;      cross-mode memory access under 1 MB until A20 is enabled.
;   4. In 32-bit mode, flip A20 via port 0x92 bit 1.
;   5. LGDT [FLAT(kernel_gdt_ptr)] - the POINTER lives in the low-
;      memory blob copy (so this read is below 1 MB), while the GDT
;      itself sits at 256 MB and is now reachable because A20 is on.
;   6. Far jump to GDT_CODE32_SEL:ap_pm_entry - boot.asm takes over
;      from here, picks a per-AP stack and heads into long mode.
;
; Position-independent: every absolute reference inside this copied
; range uses BLOB_OFF()/FLAT() so the code keeps working at 0x8000
; regardless of where the linker thought it was. Same trick as
; bios_call.asm.

bits 16

%include "src/boot/macros.asm"

section .text

; The slot SIPI will wake us at. Must be page-aligned (SIPI vector *
; 0x1000 is where the AP's CS base ends up) and in low memory so real
; mode can reach it. 0x8000 is well clear of VMBR's own low-memory
; structures (BIOS trampoline at 0x1000, E820 map at 0x2000, MBR at
; 0x7C00) and SeaBIOS's EBDA (0x9FC00+).
AP_TRAMPOLINE_ADDR equ 0x8000

%define BLOB_OFF(label) ((label) - ap_trampoline_start)
%define FLAT(label) (AP_TRAMPOLINE_ADDR + BLOB_OFF(label))

extern _gdt
extern GDT_BYTES
extern GDT_CODE32_SEL
extern ap_pm_entry

global ap_trampoline_start
global ap_trampoline_end

; Trampoline address as a C-visible dword so smp.c can reach it via
; `extern` without redefining the constant; NASM's `global X equ Y`
; doesn't actually export a value.
section .data
global ap_trampoline_addr_val
ap_trampoline_addr_val:
    dd AP_TRAMPOLINE_ADDR

section .text
ap_trampoline_start:
    cli

    ; DS/ES/SS=0 for flat physical addressing; SP to a known slot just
    ; below the trampoline so the far-return pushes don't scribble.
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, AP_TRAMPOLINE_ADDR - 0x10

    ; Load a minimal blob-local GDT (null + code32 + data32) - enough to
    ; bootstrap into 32-bit mode without having to access 256 MB first.
    lgdt [FLAT(ap_gdt_ptr)]

    ; Enable PE. The SDM asks for the very next instruction to be a
    ; far jump that reloads CS, so get straight to it.
    mov eax, cr0
    or al, 1
    mov cr0, eax

    ; o32 jmp far ptr [cs:FLAT(far_ptr)] - manual emission because
    ; NASM's 16-bit `jmp dword SEL:OFFSET` is finicky and push+retfd
    ; through the real-mode SS crashed the whole machine immediately
    ; on the SIPI delivery (KVM shutdown path).
    db 0x66, 0xFF, 0x2E         ; o32 jmp far ptr [disp16]
    dw FLAT(far_ptr)

align 4
far_ptr:
    dd FLAT(pm_landing)         ; 32-bit offset (runtime, low memory)
    dw AP_LOCAL_CODE32_SEL      ; selector into the blob-local GDT

bits 32
pm_landing:
    ; Enable A20 via PS/2 Fast A20 gate. Bit 1 opens the gate; bit 0
    ; stays clear so we don't also trigger a fast CPU reset.
    in al, 0x92
    or al, 0x02
    out 0x92, al

    ; Now load the kernel's full GDT - all the 4 GB flat segments the
    ; rest of the boot path depends on. The POINTER is addressed via
    ; FLAT() (low-memory copy) so even if A20 somehow didn't stick,
    ; the LGDT operand read doesn't wrap; the GDT it points at lives
    ; at 256 MB and needs A20 to be reachable.
    lgdt [FLAT(kernel_gdt_ptr)]

    ; Far jump into boot.asm's ap_pm_entry under the kernel GDT. From
    ; there we pick a per-AP stack and enter long mode.
    jmp dword GDT_CODE32_SEL:ap_pm_entry

align 2
kernel_gdt_ptr:
    dw GDT_BYTES - 1
    dd _gdt

; Minimal blob-local GDT: null, code32 (base=0, limit=4GB, 32-bit),
; data32 (base=0, limit=4GB). Only used for the real -> 32-bit
; transition; pm_landing re-LGDTs the kernel's full GDT before leaving
; the blob.
align 8
ap_local_gdt:
    dq 0
    dq 0x00CF9A000000FFFF       ; code32: type 0x9A, G=1, D=1, limit=0xFFFFF
    dq 0x00CF92000000FFFF       ; data32: type 0x92, G=1, D=1
ap_local_gdt_end:

AP_LOCAL_CODE32_SEL equ 0x08

align 2
ap_gdt_ptr:
    dw ap_local_gdt_end - ap_local_gdt - 1
    dd FLAT(ap_local_gdt)

ap_trampoline_end:
