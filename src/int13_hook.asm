; int13_hook.asm
; The guest half of the INT 13h (BIOS disk) hook. The host
; (installInt13Hook in hooks.c) copies this stub into guest memory and
; points the guest's INT 13h vector at it. It runs in the guest, in real
; mode, with CS = HOOK13_STUB_SEGMENT.
;
; This is an OBSERVE-then-CHAIN hook, not a service hook: for every disk
; call it bounces into the host with a VMCALL - carrying a disk-specific
; magic in EBP so the host can tell it apart from the E820 hypercall and
; from a guest executing VMCALL on its own (which gets a #UD) - and then
; it chains to the original BIOS handler, which performs the real read
; and returns straight to the caller. The host only looks; it never
; touches the guest's registers or the data, so the actual I/O still goes
; through the real BIOS exactly as it would unhooked.
;
; Why observe-and-chain rather than service it in the host: once VMXON
; has run, the host can no longer issue its own INT 13h (reaching real
; mode needs CR0.PE=0, which VMX operation forbids), so the real read has
; to stay with the guest's BIOS. The hook's value is that VMBR now sees
; every disk access the firmware boot chain makes.
;
; Position-independent: copied before it runs, every self reference is
; CS-relative, so the one fixed assumption is CS = the copy's segment,
; which the IVT entry the host writes guarantees.

bits 16

section .text

global int13_hook_start
global int13_hook_end
global int13_hook_original_vector

VMBR_DISK_MAGIC equ 0x564D4244   ; "VMBD" - distinct from the E820 "VMBR"

int13_hook_start:
    ; EBP is not an INT 13h argument, so it is free to carry the magic;
    ; save and restore it so every disk-call register (AH function, DL
    ; drive, DS:SI DAP / ES:BX buffer, CX, ...) reaches the real handler
    ; byte-for-byte untouched. The host reads the call out of the guest
    ; registers, logs it, advances past the VMCALL, and resumes us here.
    push ebp
    mov ebp, VMBR_DISK_MAGIC
    vmcall
    pop ebp

    ; Chain to the original handler. The INT that reached us already
    ; pushed the caller's FLAGS/CS/IP; the real handler IRETs on that
    ; frame and returns to the caller directly, with its own status.
    jmp far [cs:int13_hook_original_vector - int13_hook_start]

; Patched by installInt13Hook with the original INT 13h vector (offset
; then segment) saved before the hook overwrote it.
int13_hook_original_vector:
    dw 0    ; offset
    dw 0    ; segment

int13_hook_end:
