; bios_call.asm
; A mechanism for calling real-mode BIOS interrupts from long mode.
; BIOS interrupts only work in real mode, and real-mode segment:offset
; addressing can't reach our kernel's normal load address (1MB+), so
; the whole round trip - long mode -> 32-bit compatibility mode ->
; 16-bit protected mode -> real mode -> [BIOS call] -> back up through
; the same stages - is written to be *copied* to a fixed low address
; (TRAMPOLINE_ADDR) and executed from there.
;
; Every absolute reference *inside* the copied range uses FLAT()/
; BLOB_OFF() below (a compile-time "TRAMPOLINE_ADDR + offset-within-
; the-blob" computation) instead of a label's own linked address - the
; linked address is wherever this file ended up in the normal ~1MB+
; kernel image, which is NOT where the bytes actually run from once
; copied down to TRAMPOLINE_ADDR. The one exception is 64-bit code,
; which uses ordinary RIP-relative addressing ([rel label]) - that
; works unmodified after the copy because it's relative to wherever
; the code is *currently* executing, not a fixed link-time address.
;
; Mode-switch technique: every transition through a GDT code segment
; below uses "push selector; push offset; retf<size>" instead of a
; jmp. Two earlier attempts using "jmp far [mem]" (indirect, reading a
; pointer from memory) both broke on addressing-mode ambiguities that
; only showed up in disassembly, not at assemble time - NASM's default
; pointer size for that form depends on context in ways that are easy
; to get wrong (confirmed: one defaulted to a 64-bit-sized pointer
; when a 32-bit one was stored there, another picked register-indirect
; addressing over a fixed address because the address happened to fit
; in 16 bits). push+retf sidesteps all of that: there's no memory
; operand at all, just two immediate pushes and one far return, so the
; only thing that has to match is the push width vs the retf's own
; suffix - both of which are chosen here, not inferred by the
; assembler. It's also the *only* way to do a 64-bit -> 32-bit far
; jump at all, since the direct far-jump opcode doesn't exist in
; 64-bit mode.
;
; retf's size suffix must match how many bytes were actually pushed
; (i.e. the *current*/source mode's natural push width), not the
; target mode - the target segment's own descriptor (its L/D bits,
; from gdt.asm) is what actually determines the resulting mode.
;
; The real-mode payload itself: an int 15h/eax=0xE820 loop that fills
; MEMORY_MAP_ENTRIES_ADDR (see memory_map.asm) with the BIOS's view of
; physical memory - the round trip mechanism above was proven first
; with a trivial serial character print before this replaced it.

bits 64

%include "src/boot/memory_map.asm"

extern GDT_CODE32_SEL
extern GDT_DATA32_SEL
extern GDT_CODE16_SEL
extern GDT_CODE64_SEL
extern GDT_DATA64_SEL

global bios_load_memory_map
global bios_read_disk

; 0x8000 (the original choice) sits in the 0x7E00-0x9FBFF range that's
; conventionally "free" but NOT the specifically BIOS-trusted zone real
; bootloaders relocate themselves to - and it's not trusted for a good
; reason: some BIOS service routines (this one included, apparently)
; use "free" memory above 0x7C00 as their own transient scratch space
; during a call, which silently corrupted our saved_rsp mid-flight
; (confirmed via gdb: repeated int 15h calls, each entering real BIOS
; code fine, but the final return-to-long-mode landed at a garbage,
; code-size-dependent offset - exactly what a clobbered saved_rsp
; produces). 0x1000 is the classic "safe" low address every real
; bootloader trusts for exactly this reason.
TRAMPOLINE_ADDR   equ 0x1000
; Chosen so SEGMENT*16 == TRAMPOLINE_ADDR - every in-blob real-mode
; offset is then just its own offset from bios_trampoline_start.
REAL_MODE_SEGMENT equ TRAMPOLINE_ADDR >> 4
REAL_MODE_STACK   equ 0x7C00                 ; grows down, well above the (now relocated) trampoline

%define BLOB_OFF(label) ((label) - bios_trampoline_start)
%define FLAT(label) (TRAMPOLINE_ADDR + BLOB_OFF(label))

section .text

; void bios_load_memory_map(void)
; Copies the trampoline to TRAMPOLINE_ADDR and runs it, which fills
; MEMORY_MAP_COUNT_ADDR/MEMORY_MAP_ENTRIES_ADDR with the BIOS E820
; memory map. An ordinary 64-bit call/ret from the caller's side - by
; the time this returns, we're back in long mode on the normal
; GDT/paging/stack, with the map sitting at those fixed addresses
; (identity-mapped, so a plain pointer read works).
bios_load_memory_map:
    push rsi
    push rdi
    push rcx

    lea rsi, [rel bios_trampoline_start]
    mov rdi, TRAMPOLINE_ADDR
    mov rcx, bios_trampoline_end - bios_trampoline_start
    rep movsb

    pop rcx
    pop rdi
    pop rsi

    ; A direct "call TRAMPOLINE_ADDR" needs the assembler to encode a
    ; PC-relative rel32 (there's no absolute-call encoding in 64-bit
    ; mode), which the linker then has to patch once this code's final
    ; address is known - confirmed via raw byte inspection that this
    ; patching came out wrong once the whole kernel switched to an
    ; elf64 build (worked fine under the old elf32 one). Loading the
    ; absolute address into a register first needs no such patching at
    ; all - "mov r64, imm64" just encodes the literal bytes directly,
    ; unaffected by wherever this call site itself ends up linked.
    mov rax, TRAMPOLINE_ADDR
    call rax
    ret

; int bios_read_disk(uint8_t drive, dap_t *dap_ptr)
; Calls BIOS int 0x13, ah=0x42 (extended read) with the given DAP structure.
; The DAP contains the LBA sector number, number of sectors, and buffer address.
; Returns AH (BIOS error code): 0 = success, non-zero = error
; CF is converted to a return code (bit in AH) for easier C-side checking.
bios_read_disk:
    push rbx
    push rsi
    push rdi

    ; rdi = drive, rsi = dap_ptr (System V AMD64 ABI).
    ; The real-mode payload can't reach these registers (or any high
    ; kernel address) once it has dropped to 16-bit, so the two values
    ; it needs are written into slots that live *inside* the disk blob
    ; and therefore travel with it when the rep movsb below copies the
    ; blob down to low memory. Writing them to the blob's source copy
    ; here (via [rel ...]) means the copy carries the written values.
    mov [rel disk_tramp_drive], dil      ; drive number (low byte)
    mov [rel disk_tramp_dapoff], si      ; DAP offset (low 16 bits; DAP is <64KB)

    lea rsi, [rel bios_disk_trampoline_start]
    mov rdi, TRAMPOLINE_ADDR
    mov rcx, bios_disk_trampoline_end - bios_disk_trampoline_start
    rep movsb

    mov rax, TRAMPOLINE_ADDR
    call rax                             ; sets rax to AH return code

    pop rdi
    pop rsi
    pop rbx
    ret

; ===================================================================
; Copied to TRAMPOLINE_ADDR before it runs - see FLAT()/BLOB_OFF()
; above. saved_rsp lives inside this same range too, deliberately, so
; the single rep movsb above copies code and data together as one
; contiguous unit.
; ===================================================================
bios_trampoline_start:

    cli                                ; interrupts are already off
                                        ; (GRUB/our own code never
                                        ; enabled them) - be explicit
                                        ; before this dance anyway
    mov [rel saved_rsp], rsp
    mov rsp, TRAMPOLINE_ADDR - 0x10    ; tiny scratch stack for this
                                        ; stage - real mode sets up its
                                        ; own stack further down before
                                        ; it needs one (int pushes
                                        ; flags/cs/ip)

    ; ---- 64-bit long mode -> 32-bit compatibility mode ----
    ; 64-bit mode has no direct far-jump opcode at all, so this is the
    ; only way to do this transition. Natural push width here is
    ; 64-bit (long mode always pushes a full 8-byte slot for an
    ; immediate), hence retfq.
    push GDT_CODE32_SEL
    push FLAT(compat32)
    retfq

bits 32
compat32:
    ; ---- leave IA-32e mode entirely ----
    mov eax, cr0
    and eax, 0x7FFFFFFF                ; clear PG
    mov cr0, eax

    mov ecx, 0xC0000080                ; IA32_EFER
    rdmsr
    and eax, 0xFFFFFEFF                ; clear LME
    wrmsr

    mov eax, GDT_DATA32_SEL
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; ---- 32-bit protected -> 16-bit protected ----
    ; Needed so the instructions right after CR0.PE clears below are
    ; already being decoded as 16-bit *before* that happens, not just
    ; after - otherwise there's a one-instruction decode mismatch at
    ; the exact moment PE changes. Natural push width here is 32-bit
    ; (bits32's default), hence retfd.
    push GDT_CODE16_SEL
    push FLAT(prot16)
    retfd

bits 16
prot16:
    mov eax, cr0
    and al, 0xFE                       ; clear PE
    mov cr0, eax

    ; Real-mode BIOS interrupts (like the int 15h calls below) are
    ; dispatched via the IVT at physical address 0 - but the CPU still
    ; consults IDTR to find it, and IDTR is a separate register that
    ; clearing CR0.PE does NOT reset. GRUB's own protected-mode setup
    ; leaves IDTR pointing somewhere real-mode-incompatible, so every
    ; BIOS interrupt call would enter something but never correctly
    ; return without this. Explicitly reload the standard real-mode
    ; IDT (base=0, limit=0x3FF) before any BIOS interrupt can be called.
    lidt [FLAT(real_mode_idt_ptr)]

    jmp REAL_MODE_SEGMENT:BLOB_OFF(real_entry)

real_mode_idt_ptr:
    dw 0x3FF                           ; limit: 256 entries * 4 bytes - 1
    dd 0                                ; base: the IVT always lives at physical 0

real_entry:
    ; True real mode now. Segment registers don't carry over usable
    ; state across the PE=0 transition, so set them up fresh.
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov esp, REAL_MODE_STACK

    ; ---- the real-mode payload: BIOS E820 memory map ----
    ; ebx is the BIOS's continuation token: 0 to start, and it hands
    ; back a nonzero value to pass into the next call until the list
    ; is exhausted (BIOS sets it back to 0 on the final entry, or CF
    ; signals an error/end-of-list some BIOSes use instead). es:di is
    ; the output buffer - es is already 0 from above, so di alone can
    ; address the whole low-memory range this buffer lives in.
    mov di, MEMORY_MAP_ENTRIES_ADDR
    xor ebx, ebx
    xor ebp, ebp                        ; entries written so far
.e820_loop:
    xor ax, ax
    mov es, ax                          ; some BIOSes are known to not
                                         ; reliably preserve ES across
                                         ; this call - reassert it every
                                         ; iteration rather than trusting
                                         ; it survived the previous one
    mov eax, 0xE820
    mov edx, 0x534D4150                 ; 'SMAP' - required magic, BIOS echoes it back in eax
    mov ecx, MEMORY_MAP_ENTRY_SIZE
    int 0x15
    jc .e820_done                       ; CF set -> error, or genuinely done
    cmp eax, 0x534D4150
    jne .e820_done                      ; BIOS didn't echo SMAP back -> not a real entry
    inc ebp
    add di, MEMORY_MAP_ENTRY_SIZE
    cmp ebp, MEMORY_MAP_MAX_ENTRIES
    jae .e820_done                      ; buffer full - stop even if the BIOS has more
    test ebx, ebx
    jnz .e820_loop                      ; ebx=0 from the BIOS means that was the last entry
.e820_done:
    mov dword [MEMORY_MAP_COUNT_ADDR], ebp

    ; ---- real mode -> 16-bit protected mode ----
    mov eax, cr0
    or al, 1                           ; set PE
    mov cr0, eax

    ; Still being decoded as 16-bit here, whose natural push width is
    ; 16-bit - but we want 32-bit-wide pushes (avoids relying on a
    ; 16-bit relocation for the extern selector/address, and matches
    ; retfd below). The "o32" instruction prefix alone only emits the
    ; 0x66 operand-size override byte - it doesn't force the immediate
    ; itself to be encoded as 4 bytes, so NASM still under-sized it
    ; (confirmed via raw byte inspection: it wrote a corrupted 3-byte
    ; immediate that bled into the next instruction's prefix byte).
    ; "dword" is what actually forces a full 4-byte immediate here.
    push dword GDT_CODE32_SEL
    push dword FLAT(prot32)
    retfd

bits 32
prot32:
    mov eax, GDT_DATA32_SEL
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; ---- 32-bit protected -> re-enter long mode ----
    ; CR3 was never touched during any of this, so it's still
    ; correctly pointing at pml4_table - nothing to reload there.
    mov eax, cr4
    or eax, 1 << 5                     ; PAE (already set, but be explicit)
    mov cr4, eax

    mov ecx, 0xC0000080
    rdmsr
    or eax, 1 << 8                     ; LME
    wrmsr

    mov eax, cr0
    or eax, 1 << 31                    ; PG
    mov cr0, eax

    ; Natural push width here is 32-bit (bits32's default) - retf's
    ; suffix matches that, not the 64-bit target we're landing in;
    ; GDT_CODE64_SEL's own L=1 descriptor is what actually puts us in
    ; 64-bit mode.
    push GDT_CODE64_SEL
    push FLAT(long64)
    retfd

bits 64
long64:
    mov eax, GDT_DATA64_SEL
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    mov rsp, [rel saved_rsp]
    ret                                 ; pops the return address that
                                        ; bios_load_memory_map's own
                                        ; "call TRAMPOLINE_ADDR" pushed
                                        ; - resumes there

saved_rsp:
    dq 0

bios_trampoline_end:

; ===================================================================
; Disk read trampoline - runs in real mode to call int 0x13.
;
; This is a SEPARATE blob from bios_trampoline, copied down to
; TRAMPOLINE_ADDR on its own. That means its internal absolute
; references must be computed relative to THIS blob's start, not
; bios_trampoline_start - so it has its own DFLAT()/DBLOB_OFF() macros.
; Using the shared FLAT()/BLOB_OFF() here (anchored to the other blob)
; was the original bug: every mode-switch target and data reference
; landed a whole blob-length off and the CPU reset mid-transition.
;
; Everything the real-mode payload needs - the drive number, the DAP
; offset, the IDT pointer, the result byte, the saved RSP - lives
; inside this blob too, so one rep movsb brings it all into low memory
; where 16-bit code can actually reach it. The high kernel copy of
; disk_read_params (the previous approach) was unreachable from real
; mode.
; ===================================================================
%define DBLOB_OFF(label) ((label) - bios_disk_trampoline_start)
%define DFLAT(label) (TRAMPOLINE_ADDR + DBLOB_OFF(label))

bios_disk_trampoline_start:

    cli
    mov [rel disk_saved_rsp], rsp     ; RIP-relative: we're already running
                                       ; the low copy, so this hits the low
                                       ; copy's slot (read back the same way
                                       ; at the end)
    mov rsp, TRAMPOLINE_ADDR - 0x10

    ; ---- 64-bit long mode -> 32-bit compatibility mode ----
    push GDT_CODE32_SEL
    push DFLAT(disk_compat32)
    retfq

bits 32
disk_compat32:
    ; ---- leave IA-32e mode entirely ----
    mov eax, cr0
    and eax, 0x7FFFFFFF                ; clear PG
    mov cr0, eax

    mov ecx, 0xC0000080                ; IA32_EFER
    rdmsr
    and eax, 0xFFFFFEFF                ; clear LME
    wrmsr

    mov eax, GDT_DATA32_SEL
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; ---- 32-bit protected -> 16-bit protected ----
    push GDT_CODE16_SEL
    push DFLAT(disk_prot16)
    retfd

bits 16
disk_prot16:
    mov eax, cr0
    and al, 0xFE                       ; clear PE
    mov cr0, eax

    ; Reload the real-mode IVT descriptor before any BIOS interrupt
    ; (this blob's own copy - see the identical step in the E820 blob).
    lidt [DFLAT(disk_real_mode_idt_ptr)]

    jmp REAL_MODE_SEGMENT:DBLOB_OFF(disk_real_entry)

disk_real_mode_idt_ptr:
    dw 0x3FF                           ; limit: 256 * 4 - 1
    dd 0                                ; base: IVT at physical 0

disk_real_entry:
    ; True real mode - set up segments
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov esp, REAL_MODE_STACK

    ; ---- BIOS int 0x13, ah=0x42 (extended read sectors) ----
    ; DS:SI must point at the DAP. DS=0 (just set), and disk_tramp_dapoff
    ; holds DAP_ADDRESS (<64KB), so DS:SI = 0x0000:DAP_ADDRESS. DL is the
    ; drive number. Both were copied in with the blob.
    mov dl, [DFLAT(disk_tramp_drive)]          ; drive number
    mov si, [DFLAT(disk_tramp_dapoff)]         ; DAP offset

    mov ah, 0x42                               ; Extended Read Sectors
    int 0x13

    ; AH holds the status; CF set on error. Record a single byte: 0 on
    ; success, else the BIOS code (or 1 if only CF told us it failed).
    mov [DFLAT(disk_result)], ah
    jnc .disk_read_ok
    cmp ah, 0
    jne .disk_read_ok
    mov byte [DFLAT(disk_result)], 1
.disk_read_ok:

    ; ---- real mode -> 32-bit protected mode ----
    mov eax, cr0
    or al, 1                           ; set PE
    mov cr0, eax

    push dword GDT_CODE32_SEL
    push dword DFLAT(disk_prot32)
    retfd

bits 32
disk_prot32:
    mov eax, GDT_DATA32_SEL
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; ---- 32-bit protected -> re-enter long mode ----
    mov eax, cr4
    or eax, 1 << 5                     ; PAE
    mov cr4, eax

    mov ecx, 0xC0000080
    rdmsr
    or eax, 1 << 8                     ; LME
    wrmsr

    mov eax, cr0
    or eax, 1 << 31                    ; PG
    mov cr0, eax

    push GDT_CODE64_SEL
    push DFLAT(disk_long64)
    retfd

bits 64
disk_long64:
    mov eax, GDT_DATA64_SEL
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Return the error code from disk read in RAX
    movzx eax, byte [rel disk_result]

    mov rsp, [rel disk_saved_rsp]
    ret

; ---- in-blob data: copied down with the code, reachable from real mode ----
disk_saved_rsp:
    dq 0
disk_tramp_drive:
    db 0
disk_tramp_dapoff:
    dw 0
disk_result:
    db 0

bios_disk_trampoline_end:
