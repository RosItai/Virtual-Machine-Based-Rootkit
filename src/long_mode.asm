; long_mode.asm
; The actual switch into 64-bit long mode. Everything before this
; (paging.asm, gdt.asm) only prepared the pieces; this is where they
; get turned on, in the order the CPU requires:
;   1. CR4.PAE=1      - enable PAE, required for the PML4-style tables
;                       setup_paging (paging.asm) already built
;   2. CR3=pml4_table - point the CPU at those tables
;   3. EFER.LME=1     - set Long Mode Enable, via the EFER MSR
;   4. CR0.PG=1       - turn paging on. The CPU is now in IA-32e
;                       *compatibility* submode: long mode is active,
;                       but we're still executing 32-bit code, because
;                       CS still points at GDT_CODE32_SEL (gdt.asm).
;   5. far jump to GDT_CODE64_SEL - this is what actually switches
;                       instruction decoding to 64-bit.

bits 32

extern pml4_table
extern GDT_CODE64_SEL
extern GDT_DATA64_SEL

global enter_long_mode

CR4_PAE  equ 1 << 5
CR0_PG   equ 1 << 31
EFER_MSR equ 0xC0000080
EFER_LME equ 1 << 8

; void enter_long_mode(void)
; Never returns - lands in long_mode_entry below and halts there.
enter_long_mode:
    mov eax, cr4
    or eax, CR4_PAE
    mov cr4, eax

    mov eax, pml4_table
    mov cr3, eax

    mov ecx, EFER_MSR
    rdmsr
    or eax, EFER_LME
    wrmsr

    mov eax, cr0
    or eax, CR0_PG
    mov cr0, eax

    ; A direct "jmp SEL:offset" needs the selector as an immediate
    ; baked in at assemble time - but GDT_CODE64_SEL only gets its
    ; value from gdt.asm at *link* time, and that's an unreliable
    ; combination (instruction-embedded selector relocations across
    ; object files are the classic way this "mostly works" and then
    ; silently jumps to a garbage selector -> #GP -> triple fault,
    ; since there's no IDT yet to catch it). Going through an ordinary
    ; in-memory far pointer instead only needs plain data relocations,
    ; which are unambiguous.
    jmp far [long_mode_ptr]

section .data
align 4
long_mode_ptr:
    dd long_mode_entry
    dw GDT_CODE64_SEL

section .text
; ---------------------------------------------------------------------
; From here on the CPU is decoding 64-bit instructions. A brief asm
; confirmation message, then the BIOS memory-map call (still needs to
; happen before any C reads MEMORY_MAP_TABLE_ADDR), then control hands
; off to kernel_main - genuine 64-bit C from here on.
; ---------------------------------------------------------------------
extern bios_load_memory_map
extern kernel_main

bits 64
long_mode_entry:
    ; Same reasoning as above: load the selector through a 32-bit mov
    ; (a well-trodden relocation path) rather than a 16-bit immediate,
    ; then only use its low 16 bits.
    mov eax, GDT_DATA64_SEL
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; _start's "call enter_long_mode" pushed a 4-byte return address
    ; (32-bit code) that's never popped - enter_long_mode jumps here
    ; instead of returning - permanently skewing RSP by 4 bytes versus
    ; what every 64-bit "call" from here on assumes. The x86-64 ABI
    ; requires RSP to be 16-byte aligned at a call site (so a compiled
    ; function's own prologue ends up aligned for SSE instructions);
    ; without this, kernel_main (real GCC-compiled code, unlike the
    ; hand-written asm calls above it) could #GP the moment it touches
    ; anything 16-byte-aligned. Nothing on the current stack is needed
    ; past this point, so a plain realign is enough.
    and rsp, -16

    lea rsi, [rel msg_ok]
    call serial_print64

    ; bios_load_memory_map (bios_call.asm) round-trips down to real
    ; mode, runs the BIOS E820 call in a loop, and comes back here -
    ; an ordinary call/ret; if it returns at all instead of
    ; triple-faulting, the transition succeeded. The map itself is
    ; left at MEMORY_MAP_COUNT_ADDR/MEMORY_MAP_ENTRIES_ADDR
    ; (identity-mapped low memory, so plain pointer reads work) -
    ; kernel_main reads it directly as a MemoryMapTable* from there
    ; (see include/memory_map.h), so nothing further needs printing
    ; from asm here.
    call bios_load_memory_map

    call kernel_main

.hang:
    hlt
    jmp .hang

COM1 equ 0x3F8

; void serial_print64(void) - in: rsi = pointer to a null-terminated string
serial_print64:
.next_char:
    mov al, [rsi]
    test al, al
    jz .done
    call serial_putc64
    inc rsi
    jmp .next_char
.done:
    ret

; void serial_putc64(void) - in: al = character to send
serial_putc64:
    push rax                ; save the character - the wait loop below needs al too
.wait:
    mov dx, COM1 + 5
    in al, dx
    test al, 0x20
    jz .wait
    pop rax                 ; restore the character
    mov dx, COM1
    out dx, al
    ret

section .rodata
msg_ok: db 13, 10, "[long mode active - 64-bit code executing]", 13, 10, 0
