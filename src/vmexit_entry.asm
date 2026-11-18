; vmexit_entry.asm
; The VM-exit entry point - what HOST_RIP points at (initializeHostState
; in vmx.c). The CPU jumps straight here out of the guest, and this is
; the only place that can bridge an exit into compiled C, because of
; what a VM exit does *not* do for us:
;
;   - It does not save a single general-purpose register. Every GPR
;     still holds the guest's value when we arrive, and the first
;     compiled C function's prologue would overwrite several of them,
;     so they get spilled here first and restored before VMRESUME.
;     (RSP is the exception: the CPU loads it from HOST_RSP, and the
;     guest's own RSP is in the VMCS, not on this stack.)
;   - It does not push a return address. Getting back to the guest is
;     VMRESUME, not RET.
;   - It does not align the stack for us. HOST_RSP is 16-byte aligned,
;     but the 15 pushes below leave it off by 8, and the x86-64 SysV
;     ABI requires 16-byte alignment at a call site - the same trap
;     that bit the original 32-bit -> 64-bit handoff in long_mode.asm,
;     with the same symptom (a #GP inside GCC-generated code on its
;     first 16-byte-aligned access, nowhere near the actual mistake).

bits 64

section .text

extern handleVmExit
extern vmResumeFailed

global vmexit_entry

vmexit_entry:
    ; Spill the guest's GPRs. The order here defines the layout of
    ; GuestRegisters (include/vmexit.h) - last pushed is lowest in
    ; memory, so the struct lists r15 first and rax last.
    push rax
    push rcx
    push rdx
    push rbx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov rdi, rsp                ; GuestRegisters * - first argument
    mov rbp, rsp                ; rbp is callee-saved, so C preserves it
    and rsp, -16                ; align for the call, per the SysV ABI
    call handleVmExit
    mov rsp, rbp                ; back to the spilled registers

    ; The handler may have modified these (CPUID results, an MSR read),
    ; so they go back to the guest as they stand now.
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rbx
    pop rdx
    pop rcx
    pop rax

    vmresume

    ; Only reached if VMRESUME failed - on success it transfers control
    ; to the guest and never comes back here. Capture RFLAGS before
    ; anything else touches them: the stack realign below is itself an
    ; AND, which would overwrite the CF/ZF that say why it failed.
    pushfq
    pop rdi
    and rsp, -16
    call vmResumeFailed
.hang:
    cli
    hlt
    jmp .hang
