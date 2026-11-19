; int15_hook.asm
; The guest half of the E820 hook: a tiny real-mode stub that the host
; (installGuestHooks in hooks.c) copies into guest memory at physical
; 0x8000 and points the guest's INT 15h vector at. It runs in the
; guest, in real mode, with CS = 0x0800.
;
; For an E820 query it bounces into the host with a VMCALL - carrying
; the VMBR magic in EBP so the host can tell this apart from a guest
; executing VMCALL on its own (which gets a #UD) - and the host fills in
; the answer. For anything else it chains to the original BIOS handler,
; whose far pointer the host patched into int15_hook_original_vector
; after copying the blob.
;
; Position-independent: it's copied before it runs and every self
; reference is CS-relative, so the one fixed assumption is CS = the
; copy's segment (0x0800), which the IVT entry the host writes
; guarantees.

bits 16

section .text

global int15_hook_start
global int15_hook_end
global int15_hook_original_vector

E820_FUNCTION  equ 0xE820
VMBR_MAGIC     equ 0x564D4252   ; "VMBR"

int15_hook_start:
    cmp eax, E820_FUNCTION
    jne .chain

    ; --- our E820 hook ---
    ; EBP isn't an INT 15h argument, so it's free to carry the magic;
    ; save and restore it so the caller sees it preserved, exactly as a
    ; real BIOS call would. The host sets EAX/EBX/ECX, writes the entry
    ; to ES:DI, and sets CF in the live flags before returning here.
    push ebp
    mov ebp, VMBR_MAGIC
    vmcall
    pop ebp

    ; INT pushed FLAGS, CS, IP; "retf 2" returns to the caller and drops
    ; that saved FLAGS word, so the live CF the host just set survives as
    ; the call's status - a plain IRET would restore the caller's flags
    ; and lose it. STI first: INT cleared IF, and the caller expects
    ; interrupts back on (it takes effect after the next instruction, so
    ; after the return).
    sti
    retf 2

.chain:
    ; Not an E820 query - hand it to the real BIOS handler untouched.
    jmp far [cs:int15_hook_original_vector - int15_hook_start]

; Patched by installGuestHooks with the original INT 15h vector
; (offset then segment) saved before the hook overwrote it.
int15_hook_original_vector:
    dw 0    ; offset
    dw 0    ; segment

int15_hook_end:
