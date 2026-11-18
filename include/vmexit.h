/* vmexit.h - the VM-exit path: see vmexit_entry.asm (the entry stub) and
 * vmexit.c (the dispatcher and the per-reason handlers).
 */
#ifndef VMEXIT_H
#define VMEXIT_H

#include <stdint.h>

/* The guest's general-purpose registers, exactly as vmexit_entry.asm pushes
 * them. A VM-exit does NOT save GPRs anywhere - on entry to the host
 * every one of them still holds the guest's value, so the asm stub has
 * to spill them before any compiled C code runs and touches them. The
 * member order is the reverse of the push order (last pushed sits at
 * the lowest address, which is where RSP points), so changing one
 * without the other silently scrambles the guest's registers.
 *
 * RSP is deliberately absent: the guest's stack pointer lives in the
 * VMCS (GUEST_RSP), not on the host stack. */
typedef struct {
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t r11;
    uint64_t r10;
    uint64_t r9;
    uint64_t r8;
    uint64_t rdi;
    uint64_t rsi;
    uint64_t rbp;
    uint64_t rbx;
    uint64_t rdx;
    uint64_t rcx;
    uint64_t rax;
} GuestRegisters;


typedef struct {
    GuestRegisters *registers;

    uint32_t exitReason;        /* basic reason, VM_EXIT_REASON bits 15:0 */
    int entryFailure;           /* VM_EXIT_REASON bit 31: entry failed, guest never ran */
    uint64_t exitQualification;

    uint64_t guestLinearAddress;
    uint64_t guestPhysicalAddress;

    uint32_t interruptionInfo;      /* VM_EXIT_INTR_INFO - for an exception/NMI exit */
    uint32_t interruptionErrorCode;

    /* An event (interrupt/exception) that was mid-delivery through the
     * guest's IDT when the exit cut in. If valid, it has to be
     * re-injected or it's silently lost - see the dispatcher. */
    uint32_t idtVectoringInfo;
    uint32_t idtVectoringErrorCode;

    uint32_t instructionLength;     /* bytes, to step GUEST_RIP past the faulting insn */
    uint32_t instructionInfo;       /* VMX_INSTRUCTION_INFO - operand decode */
    uint32_t vmxInstructionError;   /* VM_INSTRUCTION_ERROR */
} VmExitContext;

/* The VM-exit entry point, written into HOST_RIP. Never called from C -
 * the CPU itself jumps here on every VM exit. */
extern void vmexit_entry(void);

/* Called by vmexit_entry with the spilled registers. Handles the exit
 * and returns; the stub then restores the (possibly modified)
 * registers and VMRESUMEs. Anything it can't handle it reports and
 * halts in, rather than resuming a guest whose state is now wrong. */
void handleVmExit(GuestRegisters *regs);

/* Called by vmexit_entry if VMRESUME itself failed, with the RFLAGS
 * captured immediately after the instruction. */
void vmResumeFailed(uint64_t rflags);

/* Writes a guest CR0/CR4 the way the exit handler does: the requested
 * value forced into whatever the FIXED0/FIXED1 MSRs demand, the
 * unforced value left in the read shadow, and the IA32e-mode-guest
 * entry control brought back into agreement. vmx.c uses these for the
 * initial state too, so there's exactly one place that knows the
 * rules. */
void writeGuestCr0(uint64_t value);
void writeGuestCr4(uint64_t value);

/* Dumps the guest-state and control fields most often responsible for
 * a failed VM entry. Used by the handlers and by vmx.c's launch path. */
void dumpVmcsState(void);

#endif
