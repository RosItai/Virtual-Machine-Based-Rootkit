; guest.asm
; The real-mode guest: 16-bit code that InitializeVmm (vmx.c) copies
; below 1MB and launches with CR0.PE=0 - possible only because the
; unrestricted guest control is on. It prints straight to COM1 with
; port I/O and never calls host code directly, so its messages appearing
; prove the guest itself executed in real mode.
;
; It exercises two host facilities it can't see directly:
;   - CPUID exits unconditionally in every mode, so reaching the
;     "resumed" message proves the whole VM-exit path (save registers,
;     dispatch, restore, VMRESUME).
;   - INT 15h / E820 is hooked by the host (hooks.c): the map it prints
;     is the real BIOS map with the VMM's own memory turned reserved, so
;     a reserved hole appearing mid-RAM proves the hook ran.
;
; Position-independent: it's copied before it runs, and the host
; launches it with CS base = the copy address and IP = 0, so every
; address below is relative to real_mode_guest_start.

bits 16

section .text

global real_mode_guest_start
global real_mode_guest_end

COM1 equ 0x3F8
E820_FUNCTION equ 0xE820
SMAP_SIGNATURE equ 0x534D4150   ; "SMAP"

real_mode_guest_start:
    mov ax, cs
    mov ds, ax
    mov es, ax                      ; ES:DI is the E820 buffer below

    mov si, message - real_mode_guest_start
    call print_string

    ; A round trip through the host: CPUID exits, the host emulates it
    ; and writes the results back into the guest's saved registers, and
    ; VMRESUME returns us here.
    mov eax, 1
    cpuid

    mov si, message_resumed - real_mode_guest_start
    call print_string

    ; --- Walk the (hooked) E820 map and print every entry ---
    mov si, message_e820 - real_mode_guest_start
    call print_string

    xor ebx, ebx                    ; continuation value, 0 to start
.e820_loop:
    mov eax, E820_FUNCTION
    mov edx, SMAP_SIGNATURE
    mov ecx, 24
    mov di, e820_buffer - real_mode_guest_start
    int 0x15
    jc .e820_done                   ; CF set = error or end of map
    cmp eax, SMAP_SIGNATURE         ; BIOS echoes the signature on success
    jne .e820_done

    push ebx                        ; the print helpers don't preserve it
    mov al, ' '
    call putc
    mov al, ' '
    call putc
    mov eax, [e820_buffer - real_mode_guest_start]       ; base, low 32 bits
    call put_hex32
    mov al, ' '
    call putc
    mov al, ' '
    call putc
    mov al, [e820_buffer - real_mode_guest_start + 16]    ; type (1=usable, 2=reserved, ...)
    call put_type
    mov al, 13
    call putc
    mov al, 10
    call putc
    pop ebx

    test ebx, ebx                   ; 0 = that was the last entry
    jnz .e820_loop
.e820_done:

    mov si, message_done - real_mode_guest_start
    call print_string

.halt:
    cli
    hlt
    jmp .halt

; ---------------------------------------------------------------------
; Helpers. The call/ret path needs a stack, which the host provides -
; SS=0, SP=0x7000 (REAL_MODE_STACK_POINTER in vmx.c), growing down away
; from this code rather than into it.
; ---------------------------------------------------------------------

; in: si = offset of a null-terminated string (ds = cs)
print_string:
.next_char:
    lodsb                           ; al = ds:[si++] (DF=0, RFLAGS starts as 0x2)
    test al, al
    jz .done
    call putc
    jmp .next_char
.done:
    ret

; in: al = character. Preserves all registers.
putc:
    push ax
    push dx
    mov ah, al                      ; stash the character; the wait loop needs al
.wait:
    mov dx, COM1 + 5
    in al, dx
    test al, 0x20
    jz .wait
    mov al, ah
    mov dx, COM1
    out dx, al
    pop dx
    pop ax
    ret

; in: al = E820 type; prints its name so the map reads without a legend.
; Preserves nothing worth preserving here - the caller is done with al.
put_type:
    cmp al, 1
    je .usable
    cmp al, 2
    je .reserved
    cmp al, 3
    je .acpi_reclaim
    cmp al, 4
    je .acpi_nvs
    cmp al, 5
    je .bad
    ; unknown type - print "type " then the raw number
    push ax
    mov si, str_type - real_mode_guest_start
    call print_string
    pop ax
    call put_nibble
    ret
.usable:       mov si, str_usable       - real_mode_guest_start
               jmp print_string
.reserved:     mov si, str_reserved     - real_mode_guest_start
               jmp print_string
.acpi_reclaim: mov si, str_acpi_reclaim - real_mode_guest_start
               jmp print_string
.acpi_nvs:     mov si, str_acpi_nvs     - real_mode_guest_start
               jmp print_string
.bad:          mov si, str_bad          - real_mode_guest_start
               jmp print_string

; in: al = value; prints its low nibble as one hex digit. Preserves regs.
put_nibble:
    push ax
    and al, 0x0F
    cmp al, 10
    jb .digit
    add al, 'A' - 10
    jmp .emit
.digit:
    add al, '0'
.emit:
    call putc
    pop ax
    ret

; in: eax = value; prints all 8 hex digits, most significant first.
put_hex32:
    push eax
    push cx
    mov cx, 8
.next:
    rol eax, 4                      ; bring the next-highest nibble into al's low 4 bits
    call put_nibble
    loop .next                      ; loop uses CX in 16-bit mode
    pop cx
    pop eax
    ret

; Same look as the host's logInfo: green "[INFO] ", magenta message.
message:
    db 0x1B, "[32m[INFO] ", 0x1B, "[0m"
    db 0x1B, "[35mReal-mode guest is running!", 0x1B, "[0m", 13, 10, 0

message_resumed:
    db 0x1B, "[32m[INFO] ", 0x1B, "[0m"
    db 0x1B, "[35mGuest resumed after a CPUID VM-exit!", 0x1B, "[0m", 13, 10, 0

message_e820:
    db 0x1B, "[32m[INFO] ", 0x1B, "[0m"
    db 0x1B, "[35mE820 map as the guest sees it (base / type):", 0x1B, "[0m", 13, 10, 0

message_done:
    db 0x1B, "[32m[INFO] ", 0x1B, "[0m"
    db 0x1B, "[35mGuest finished reading the memory map.", 0x1B, "[0m", 13, 10, 0

; E820 type names, so the map is readable without a legend.
str_usable:       db "usable", 0
str_reserved:     db "reserved", 0
str_acpi_reclaim: db "ACPI reclaimable", 0
str_acpi_nvs:     db "ACPI NVS", 0
str_bad:          db "bad memory", 0
str_type:         db "type ", 0

; 24-byte scratch buffer for the E820 entries (writable: the whole blob
; is copied into RAM before it runs).
e820_buffer:
    times 24 db 0

real_mode_guest_end:
