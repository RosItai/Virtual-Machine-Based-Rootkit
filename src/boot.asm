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
    ; ---- BSP vs AP split ----
    ; All cores land here (the WSL2/KVM setup lets APs run from GRUB
    ; without waiting for a SIPI, unlike bare metal where firmware
    ; leaves them in wait-for-SIPI). IA32_APIC_BASE (MSR 0x1B) bit 8
    ; is the BSP flag: set on the bootstrap processor, clear on every
    ; AP. Only the BSP installs the GDT and builds the page tables;
    ; APs spin until that's done, then grab their own stack and join.
    mov ecx, 0x1B
    rdmsr
    test eax, 1 << 8
    jz .ap_entry

    ; ---- BSP path ----
    mov esp, stack_top   ; dedicated BSP stack

    call load_gdt          ; take ownership of the GDT (see gdt.asm)
    call setup_paging      ; build PML4/PDPT/PD tables (see paging.asm) -
                           ; doesn't touch CR0/CR3/CR4/EFER yet

    ; With the GDT and page tables in place, APs can safely enter long
    ; mode too - the paging tables stay read-only from here on, so no
    ; race with the APs loading CR3 off them. Release the APs.
    mov dword [bsp_ready], 1

    call enter_long_mode    ; flips on PAE/CR3/LME/PG and far-jumps into
                            ; 64-bit code (long_mode.asm) - never returns

.hang:                    ; should never be reached, but just in case
    cli
    hlt
    jmp .hang

    ; ---- AP path ----
    ; APs cannot touch any shared boot state until the BSP's GDT and
    ; paging are in place. Spin on bsp_ready with `pause` to be gentle
    ; on the fetch unit (hyperthread hint that we're in a busy wait).
.ap_entry:
.ap_wait:
    pause
    cmp dword [bsp_ready], 0
    je .ap_wait

    ; Claim a per-AP stack slot. `lock xadd` is the architectural way
    ; to read-modify-write a shared counter atomically across cores:
    ; eax ends up with the pre-increment value (0, 1, 2, ...), which
    ; is our AP index. Each AP's stack is AP_STACK_SIZE bytes wide,
    ; laid out back to back in ap_stacks; the stack grows down from
    ; the TOP of our slot, i.e. (index + 1) * AP_STACK_SIZE bytes in.
    mov eax, 1
    lock xadd [ap_counter], eax
    inc eax
    shl eax, 14                 ; * AP_STACK_SIZE (16384)
    mov esp, ap_stacks
    add esp, eax

    call load_gdt               ; load the shared GDT pointer - idempotent
    call enter_long_mode        ; same transition as BSP; long_mode_entry
                                ; re-checks the BSP bit to split paths
                                ; back apart once 64-bit code starts.

section .data
align 4
; BSP releases the APs by writing 1 here. 0 means "wait" - the initial
; state is 0 (BSS clears to 0, but we keep it explicit in .data).
global bsp_ready
bsp_ready:
    dd 0

; Atomic counter the APs lock-xadd to pick a unique slot in ap_stacks.
; Lives in .data only because .bss' zero-init is enough here anyway and
; co-locating these two clarifies the handshake. Exposed to C so the
; BSP side can tell (after boot) whether any APs ever joined.
global ap_counter
ap_counter:
    dd 0

section .bss
align 16
stack_bottom:
    resb 16384            ; 16KB BSP stack
stack_top:

align 16
; Per-AP stacks, 16KB each, back to back. Sized for MAX_APS cores -
; raise it if VMBR ever runs with more.
AP_STACK_SIZE equ 16384
MAX_APS       equ 8
ap_stacks:
    resb AP_STACK_SIZE * MAX_APS
