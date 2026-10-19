; guest.asm
; The real-mode guest: 16-bit code that InitializeVmm (vmx.c) copies
; below 1MB and launches with CR0.PE=0 - possible only because the
; unrestricted guest control is on. It prints straight to COM1 with
; port I/O and never calls host code, so its message appearing proves
; the guest itself executed in real mode.
;
; Position-independent: it's copied before it runs, and the host
; launches it with CS base = the copy address and IP = 0, so every
; address below is relative to real_mode_guest_start.

bits 16

section .text

global real_mode_guest_start
global real_mode_guest_end

COM1 equ 0x3F8

real_mode_guest_start:
    mov ax, cs
    mov ds, ax
    mov si, message - real_mode_guest_start
.next_char:
    lodsb                           ; al = ds:[si++] (DF=0, RFLAGS starts as 0x2)
    test al, al
    jz .halt
    mov bl, al
    mov dx, COM1 + 5
.wait_transmit_empty:
    in al, dx
    test al, 0x20
    jz .wait_transmit_empty
    mov al, bl
    mov dx, COM1
    out dx, al
    jmp .next_char
.halt:
    cli
    hlt
    jmp .halt

; Same look as the host's logInfo: green "[INFO] ", magenta message.
message:
    db 0x1B, "[32m[INFO] ", 0x1B, "[0m"
    db 0x1B, "[35mReal-mode guest is running!", 0x1B, "[0m", 13, 10, 0

real_mode_guest_end:
