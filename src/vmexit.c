/* vmexit.c
 * The VM-exit dispatcher and the per-reason handlers, reached from
 * vmexit_entry.asm's stub.
 *
 * Which exits can reach here at all is deliberate. Some are
 * unconditional - CPUID, INVD, XSETBV and every VMX instruction exit
 * no matter what the controls say, so a guest that executes one and
 * finds nothing handling it is simply dead. The rest are here because
 * a control bit asks for them: MOV-to-CR0/CR4 (through the CR0/CR4
 * guest/host masks, see initializeControls in vmx.c) and a short list
 * of MSRs (through the MSR bitmap). Everything else the guest does -
 * ordinary memory access, port I/O to QEMU's devices, the thousands of
 * MSR reads an OS does at boot - never leaves the guest, which is the
 * point: an exit we don't need is pure overhead.
 *
 * Anything unrecognised dumps state and halts rather than resuming.
 * Resuming a guest after mishandling an exit produces a crash
 * somewhere else entirely, which is far harder to read than a halt
 * naming the reason we didn't expect.
 */
#include <stdint.h>
#include "vmexit.h"
#include "vmcs.h"
#include "cpu.h"
#include "debug.h"
#include "hooks.h"

/* ===================================================================
 * Guest register access
 * =================================================================== */

/* The GPR index VMX reports in an exit qualification is the standard
 * encoding (0=RAX, 1=RCX, ... 15=R15), in which index 4 is RSP - the
 * one register the stub didn't spill, because the guest's lives in the
 * VMCS. */
static uint64_t readGuestGpr(GuestRegisters *regs, uint32_t index) {
    switch (index) {
    case 0:  return regs->rax;
    case 1:  return regs->rcx;
    case 2:  return regs->rdx;
    case 3:  return regs->rbx;
    case 4:  return vmread(GUEST_RSP);
    case 5:  return regs->rbp;
    case 6:  return regs->rsi;
    case 7:  return regs->rdi;
    case 8:  return regs->r8;
    case 9:  return regs->r9;
    case 10: return regs->r10;
    case 11: return regs->r11;
    case 12: return regs->r12;
    case 13: return regs->r13;
    case 14: return regs->r14;
    default: return regs->r15;
    }
}

static void writeGuestGpr(GuestRegisters *regs, uint32_t index, uint64_t value) {
    switch (index) {
    case 0:  regs->rax = value; break;
    case 1:  regs->rcx = value; break;
    case 2:  regs->rdx = value; break;
    case 3:  regs->rbx = value; break;
    case 4:  vmwrite(GUEST_RSP, value); break;
    case 5:  regs->rbp = value; break;
    case 6:  regs->rsi = value; break;
    case 7:  regs->rdi = value; break;
    case 8:  regs->r8 = value; break;
    case 9:  regs->r9 = value; break;
    case 10: regs->r10 = value; break;
    case 11: regs->r11 = value; break;
    case 12: regs->r12 = value; break;
    case 13: regs->r13 = value; break;
    case 14: regs->r14 = value; break;
    default: regs->r15 = value; break;
    }
}

/* Reads everything about this exit out of the VMCS in one place, so no
 * handler has to remember which field holds what. Mirrors the
 * reference's vmexit_data_t, adapted to our GuestRegisters and naming. */
static void captureVmExitContext(VmExitContext *ctx, GuestRegisters *regs) {
    uint64_t fullReason = vmread(VM_EXIT_REASON);

    ctx->registers = regs;
    ctx->exitReason = (uint32_t)(fullReason & VM_EXIT_REASON_MASK);
    ctx->entryFailure = (fullReason & VM_EXIT_REASON_ENTRY_FAILURE) != 0;
    ctx->exitQualification = vmread(EXIT_QUALIFICATION);
    ctx->guestLinearAddress = vmread(GUEST_LINEAR_ADDRESS);
    ctx->guestPhysicalAddress = vmread(GUEST_PHYSICAL_ADDRESS);
    ctx->interruptionInfo = (uint32_t)vmread(VM_EXIT_INTR_INFO);
    ctx->interruptionErrorCode = (uint32_t)vmread(VM_EXIT_INTR_ERROR_CODE);
    ctx->idtVectoringInfo = (uint32_t)vmread(IDT_VECTORING_INFO_FIELD);
    ctx->idtVectoringErrorCode = (uint32_t)vmread(IDT_VECTORING_ERROR_CODE);
    ctx->instructionLength = (uint32_t)vmread(VM_EXIT_INSTRUCTION_LEN);
    ctx->instructionInfo = (uint32_t)vmread(VMX_INSTRUCTION_INFO);
    ctx->vmxInstructionError = (uint32_t)vmread(VM_INSTRUCTION_ERROR);
}

/* An exit caused by an instruction leaves GUEST_RIP pointing *at* that
 * instruction, not past it - resuming without this re-executes it and
 * exits again forever. Exits that aren't instruction-caused (an EPT
 * violation, an injected fault) must NOT advance it. The length comes
 * from the captured context, which is where the VMCS recorded it. */
static void advanceGuestRip(VmExitContext *ctx) {
    vmwrite(GUEST_RIP, vmread(GUEST_RIP) + ctx->instructionLength);
}

static void haltForever(void) {
    for (;;) {
        __asm__ volatile("cli\n\t"
                          "hlt");
    }
}

/* ===================================================================
 * Event injection
 * =================================================================== */

/* Hands the guest an exception on the next VM entry, as if its own
 * instruction had faulted. The guest's IDT (or real-mode IVT) handles
 * it, so RIP must stay on the faulting instruction - no advance. */
static void injectException(uint32_t vector, int hasErrorCode, uint32_t errorCode) {
    uint32_t info = vector | INTR_TYPE_HARDWARE_EXCEPTION | INTR_INFO_VALID;
    if (hasErrorCode) {
        info |= INTR_INFO_DELIVER_ERROR_CODE;
        vmwrite(VM_ENTRY_EXCEPTION_ERROR_CODE, errorCode);
    }
    vmwrite(VM_ENTRY_INTR_INFO_FIELD, info);

    /* Clear any STI/MOV-SS blocking the interrupted instruction left
     * behind: it would otherwise still be in effect at entry, and the
     * event we're injecting is meant to be delivered immediately. */
    vmwrite(GUEST_INTERRUPTIBILITY_INFO,
            vmread(GUEST_INTERRUPTIBILITY_INFO) & ~INTERRUPTIBILITY_BLOCKING_MASK);
}

/* ===================================================================
 * Control registers, and the guest's own mode switches
 * =================================================================== */

/* The guest's CR0/CR4 still have to satisfy the FIXED0/FIXED1 MSRs at
 * every VM entry, exactly like the host's did before VMXON - a guest
 * write that violates them doesn't fail the write, it fails the *next*
 * entry, with reason 0x80000021 and no hint as to which bit. So the
 * value the guest asked for gets forced into range before it reaches
 * GUEST_CRn, and the unmodified value goes into the read shadow so the
 * guest still sees what it believes it wrote. */
static uint64_t adjustGuestCr0(uint64_t value) {
    /* Unrestricted guest exempts exactly PE and PG from the FIXED0
     * requirement - that's what lets the guest run unpaged at all. */
    uint64_t fixed0 = readMsr(IA32_VMX_CR0_FIXED0_MSR) & ~(CR0_PE | CR0_PG);
    uint64_t fixed1 = readMsr(IA32_VMX_CR0_FIXED1_MSR);
    return (value | fixed0) & fixed1;
}

static uint64_t adjustGuestCr4(uint64_t value) {
    /* No exemptions here: CR4.VMXE in particular stays set in
     * GUEST_CR4 whether the guest wants it or not, which is why the
     * read shadow has to hide it (see writeGuestCr4). */
    return (value | readMsr(IA32_VMX_CR4_FIXED0_MSR)) & readMsr(IA32_VMX_CR4_FIXED1_MSR);
}

/* The guest turns long mode on itself, in the architecturally required
 * order: EFER.LME first, then CR0.PG. Three things then have to agree
 * or VM entry fails - CR0.PG, EFER.LMA, and the IA32e-mode-guest entry
 * control - and only the first of them is the guest's own doing, so
 * this runs after every write to either CR0 or EFER. */
static void syncLongModeEntryControl(void) {
    uint64_t cr0 = vmread(GUEST_CR0);
    uint64_t efer = vmread(GUEST_IA32_EFER);
    uint32_t entryControls = (uint32_t)vmread(VM_ENTRY_CONTROLS);
    int longModeActive = (cr0 & CR0_PG) && (efer & EFER_LME);

    if (longModeActive) {
        efer |= EFER_LMA;
        entryControls |= VM_ENTRY_IA32E_MODE_GUEST;
    } else {
        efer &= ~EFER_LMA;
        entryControls &= ~VM_ENTRY_IA32E_MODE_GUEST;
    }

    vmwrite(GUEST_IA32_EFER, efer);
    vmwrite(VM_ENTRY_CONTROLS, entryControls);
}

void writeGuestCr0(uint64_t value) {
    vmwrite(GUEST_CR0, adjustGuestCr0(value));
    vmwrite(CR0_READ_SHADOW, value);
    syncLongModeEntryControl();
}

void writeGuestCr4(uint64_t value) {
    vmwrite(GUEST_CR4, adjustGuestCr4(value));
    /* VMXE is ours and must never show up in a guest read: an OS that
     * sees it set assumes VMX is already in use and behaves
     * accordingly. */
    vmwrite(CR4_READ_SHADOW, value & ~CR4_VMXE);
}

/* A MOV to a control register writes 64 bits only in 64-bit mode;
 * in compatibility, protected and real mode it writes 32, and the
 * upper half of the register the guest happened to use is stale data
 * that has nothing to do with the write. Passing it through anyway
 * would put garbage into GUEST_CR0/CR4 - mostly masked off again by
 * the FIXED1 AND, but not something to rely on. */
static uint64_t crOperandValue(uint64_t value) {
    int ia32eMode = (vmread(VM_ENTRY_CONTROLS) & VM_ENTRY_IA32E_MODE_GUEST) != 0;
    int sixtyFourBitCode = (vmread(GUEST_CS_AR_BYTES) & SEG_LONG_FLAG) != 0;

    return (ia32eMode && sixtyFourBitCode) ? value : (uint32_t)value;
}

static void handleCrAccess(VmExitContext *ctx) {
    GuestRegisters *regs = ctx->registers;
    uint64_t qualification = ctx->exitQualification;
    uint32_t crNumber = CR_ACCESS_CR_NUMBER(qualification);
    uint32_t gpr = CR_ACCESS_GPR(qualification);

    switch (CR_ACCESS_TYPE(qualification)) {
    case CR_ACCESS_TYPE_MOV_TO_CR: {
        uint64_t value = crOperandValue(readGuestGpr(regs, gpr));
        switch (crNumber) {
        case 0: writeGuestCr0(value); break;
        case 3: vmwrite(GUEST_CR3, value); break;
        case 4: writeGuestCr4(value); break;
        default:
            logError("MOV to CR%u not handled (value %lx)\n", crNumber, value);
            dumpVmcsState();
            haltForever();
        }
        break;
    }

    case CR_ACCESS_TYPE_MOV_FROM_CR:
        /* Only CR3 and CR8 can get here: reads of CR0/CR4 are served
         * by the CPU from the read shadows and never exit. */
        if (crNumber == 3) {
            writeGuestGpr(regs, gpr, vmread(GUEST_CR3));
        } else {
            logError("MOV from CR%u not handled\n", crNumber);
            dumpVmcsState();
            haltForever();
        }
        break;

    case CR_ACCESS_TYPE_CLTS:
        writeGuestCr0(vmread(CR0_READ_SHADOW) & ~CR0_TS);
        break;

    case CR_ACCESS_TYPE_LMSW: {
        /* LMSW loads only CR0's low 4 bits (PE, MP, EM, TS) and can
         * set PE but never clear it. Worth handling properly rather
         * than treating it as a MOV: plenty of real-mode code still
         * enters protected mode this way. */
        uint64_t source = CR_ACCESS_LMSW_SOURCE(qualification);
        uint64_t cr0 = vmread(CR0_READ_SHADOW);
        cr0 = (cr0 & ~(CR0_MP | CR0_EM | CR0_TS)) | (source & (CR0_MP | CR0_EM | CR0_TS));
        cr0 |= (source & CR0_PE);
        writeGuestCr0(cr0);
        break;
    }
    }

    advanceGuestRip(ctx);
}

/* ===================================================================
 * MSRs
 * =================================================================== */

static void handleRdmsr(VmExitContext *ctx) {
    GuestRegisters *regs = ctx->registers;
    uint32_t msr = (uint32_t)regs->rcx;
    uint64_t value;

    switch (msr) {
    case IA32_EFER_MSR:
        /* The guest's EFER lives in the VMCS (loaded on entry, saved
         * on exit) - the physical MSR holds the host's value by the
         * time we're running. */
        value = vmread(GUEST_IA32_EFER);
        break;

    case IA32_FEATURE_CONTROL_MSR:
        /* Locked, with VMX left disabled. We don't offer nested VMX,
         * and this is the one answer an OS is guaranteed to handle
         * gracefully - it simply doesn't try to become a hypervisor. */
        value = FEATURE_CONTROL_LOCKED;
        break;

    default:
        /* Only MSRs marked in the bitmap reach us, so this is a
         * safety net. Note that a genuinely invalid MSR number would
         * #GP *here*, in the host, with no IDT to catch it. */
        value = readMsr(msr);
        break;
    }

    /* RDMSR writes EAX/EDX, zeroing the upper halves of RAX/RDX. */
    regs->rax = (uint32_t)value;
    regs->rdx = (uint32_t)(value >> 32);
    advanceGuestRip(ctx);
}

static void handleWrmsr(VmExitContext *ctx) {
    GuestRegisters *regs = ctx->registers;
    uint32_t msr = (uint32_t)regs->rcx;
    uint64_t value = ((uint64_t)(uint32_t)regs->rdx << 32) | (uint32_t)regs->rax;

    switch (msr) {
    case IA32_EFER_MSR:
        /* Goes to the VMCS, not the physical MSR: entry will load it
         * for the guest, and the host keeps its own. This is also the
         * moment a guest announces it's about to go 64-bit. */
        vmwrite(GUEST_IA32_EFER, value);
        syncLongModeEntryControl();
        break;

    case IA32_FEATURE_CONTROL_MSR:
        /* We report it locked, so a write to it faults, exactly as it
         * would on hardware where firmware got there first. */
        injectException(EXCEPTION_VECTOR_GP, 1, 0);
        return;

    default:
        writeMsr(msr, value);
        break;
    }

    advanceGuestRip(ctx);
}

/* ===================================================================
 * Unconditional exits
 * =================================================================== */

#define CPUID_FEATURES_LEAF    1u
#define CPUID_1_ECX_VMX        (1u << 5)
#define CPUID_1_ECX_SMX        (1u << 6)
#define CPUID_1_ECX_XSAVE      (1u << 26)
#define CPUID_1_ECX_HYPERVISOR (1u << 31)

static void handleCpuid(VmExitContext *ctx) {
    GuestRegisters *regs = ctx->registers;
    uint32_t leaf = (uint32_t)regs->rax;
    uint32_t subleaf = (uint32_t)regs->rcx;
    uint32_t eax, ebx, ecx, edx;

    cpuidEx(leaf, subleaf, &eax, &ebx, &ecx, &edx);

    if (leaf == CPUID_FEATURES_LEAF) {
        /* Hide VMX: a guest that sees it would try VMXON, which we'd
         * have to fail anyway. SMX goes with it, since GETSEC gets a
         * #UD from the dispatcher. The hypervisor-present bit goes too
         * - nothing here pretends to be a well-behaved announced
         * hypervisor with a vendor leaf to back it up. */
        ecx &= ~(CPUID_1_ECX_VMX | CPUID_1_ECX_SMX | CPUID_1_ECX_HYPERVISOR);
    }

    /* CPUID zero-extends its 32-bit results into the 64-bit registers. */
    regs->rax = eax;
    regs->rbx = ebx;
    regs->rcx = ecx;
    regs->rdx = edx;
    advanceGuestRip(ctx);
}

static void handleXsetbv(VmExitContext *ctx) {
    GuestRegisters *regs = ctx->registers;
    uint32_t index = (uint32_t)regs->rcx;
    uint64_t value = ((uint64_t)(uint32_t)regs->rdx << 32) | (uint32_t)regs->rax;
    uint32_t eax, ebx, ecx, edx;

    if (index != 0) {
        injectException(EXCEPTION_VECTOR_GP, 1, 0);
        return;
    }

    cpuid(CPUID_FEATURES_LEAF, &eax, &ebx, &ecx, &edx);
    if (!(ecx & CPUID_1_ECX_XSAVE)) {
        injectException(EXCEPTION_VECTOR_UD, 0, 0);
        return;
    }

    /* XSETBV needs CR4.OSXSAVE set in whatever context executes it,
     * and the host never had a reason to set it before now. Setting it
     * here (and in the VMCS, so later exits land with it on) is
     * cheaper than emulating XCR0 ourselves.
     * Known gap: an XCR0 value the CPU rejects #GPs in the *host*,
     * with no IDT to catch it. Validating against CPUID leaf 0xD
     * before executing is the fix, once a guest actually exercises
     * this. */
    if (!(readCr4() & CR4_OSXSAVE)) {
        writeCr4(readCr4() | CR4_OSXSAVE);
        vmwrite(HOST_CR4, readCr4());
    }

    xsetbv(index, value);
    advanceGuestRip(ctx);
}

/* ===================================================================
 * Diagnostics
 * =================================================================== */

/* Moved here from vmx.c's placeholder handler: the same dump is just
 * as useful for an unexpected exit as it was for a failed launch, and
 * both paths now share it. */
void dumpVmcsState(void) {
    logInfo("CR0=%lx CR4=%lx RFLAGS=%lx EFER=%lx\n",
            vmread(GUEST_CR0), vmread(GUEST_CR4), vmread(GUEST_RFLAGS), vmread(GUEST_IA32_EFER));
    logInfo("CR0 shadow=%lx CR4 shadow=%lx CR3=%lx DR7=%lx\n",
            vmread(CR0_READ_SHADOW), vmread(CR4_READ_SHADOW), vmread(GUEST_CR3), vmread(GUEST_DR7));
    logInfo("CS sel=%lx ar=%lx  SS sel=%lx ar=%lx\n",
            vmread(GUEST_CS_SELECTOR), vmread(GUEST_CS_AR_BYTES),
            vmread(GUEST_SS_SELECTOR), vmread(GUEST_SS_AR_BYTES));
    logInfo("DS sel=%lx ar=%lx  ES sel=%lx ar=%lx\n",
            vmread(GUEST_DS_SELECTOR), vmread(GUEST_DS_AR_BYTES),
            vmread(GUEST_ES_SELECTOR), vmread(GUEST_ES_AR_BYTES));
    logInfo("FS sel=%lx ar=%lx  GS sel=%lx ar=%lx\n",
            vmread(GUEST_FS_SELECTOR), vmread(GUEST_FS_AR_BYTES),
            vmread(GUEST_GS_SELECTOR), vmread(GUEST_GS_AR_BYTES));
    logInfo("TR sel=%lx ar=%lx  LDTR sel=%lx ar=%lx\n",
            vmread(GUEST_TR_SELECTOR), vmread(GUEST_TR_AR_BYTES),
            vmread(GUEST_LDTR_SELECTOR), vmread(GUEST_LDTR_AR_BYTES));
    logInfo("RIP=%lx RSP=%lx\n", vmread(GUEST_RIP), vmread(GUEST_RSP));
    logInfo("GDTR base=%lx limit=%lx  IDTR base=%lx limit=%lx\n",
            vmread(GUEST_GDTR_BASE), vmread(GUEST_GDTR_LIMIT),
            vmread(GUEST_IDTR_BASE), vmread(GUEST_IDTR_LIMIT));
    logInfo("CS/SS/DS/ES base=%lx/%lx/%lx/%lx limit=%lx/%lx/%lx/%lx\n",
            vmread(GUEST_CS_BASE), vmread(GUEST_SS_BASE), vmread(GUEST_DS_BASE), vmread(GUEST_ES_BASE),
            vmread(GUEST_CS_LIMIT), vmread(GUEST_SS_LIMIT), vmread(GUEST_DS_LIMIT), vmread(GUEST_ES_LIMIT));
    logInfo("activity=%lx interruptibility=%lx pending_dbg=%lx\n",
            vmread(GUEST_ACTIVITY_STATE), vmread(GUEST_INTERRUPTIBILITY_INFO),
            vmread(GUEST_PENDING_DBG_EXCEPTIONS));
    logInfo("sysenter cs=%lx esp=%lx eip=%lx  link_ptr=%lx\n",
            vmread(GUEST_SYSENTER_CS), vmread(GUEST_SYSENTER_ESP), vmread(GUEST_SYSENTER_EIP),
            vmread(VMCS_LINK_POINTER));
    logInfo("pin=%lx primaryProc=%lx secondaryProc=%lx exit=%lx entry=%lx\n",
            vmread(PIN_BASED_VM_EXEC_CONTROL), vmread(CPU_BASED_VM_EXEC_CONTROL),
            vmread(SECONDARY_VM_EXEC_CONTROL), vmread(VM_EXIT_CONTROLS), vmread(VM_ENTRY_CONTROLS));
    logInfo("intr_info=%lx intr_error=%lx idt_vectoring=%lx instr_len=%lx\n",
            vmread(VM_EXIT_INTR_INFO), vmread(VM_EXIT_INTR_ERROR_CODE),
            vmread(IDT_VECTORING_INFO_FIELD), vmread(VM_EXIT_INSTRUCTION_LEN));
}

void vmResumeFailed(uint64_t rflags) {
    if (rflags & CARRY_FLAG_MASK) {
        logError("vmresume failed: VMfailInvalid (no current VMCS)\n");
    } else if (rflags & ZERO_FLAG_MASK) {
        logError("vmresume failed: VMfailValid, error code %lu\n", vmread(VM_INSTRUCTION_ERROR));
    } else {
        logError("vmresume returned with no failure flag set - impossible, so the "
                 "stub's flag capture is wrong\n");
    }
    dumpVmcsState();
}

static void reportFatalExit(VmExitContext *ctx, const char *what) {
    logError("%s (exit reason %u, qualification %lx)\n",
             what, ctx->exitReason, ctx->exitQualification);
    dumpVmcsState();
    haltForever();
}

/* ===================================================================
 * The dispatcher
 * =================================================================== */

void handleVmExit(GuestRegisters *regs) {
    VmExitContext context;
    VmExitContext *ctx = &context;
    captureVmExitContext(ctx, regs);

    /* Entry-failure (VM_EXIT_REASON bit 31) means the guest never ran,
     * so resuming would just fail the same way. */
    if (ctx->entryFailure) {
        reportFatalExit(ctx, "VM-entry failed");
    }

    switch (ctx->exitReason) {
    /* --- Unconditional exits --- */
    case EXIT_REASON_CPUID:
        handleCpuid(ctx);
        return;

    case EXIT_REASON_INVD:
        /* Never pass a real INVD through: it drops dirty cache lines
         * without writing them back, including the host's. WBINVD has
         * the effect the guest is actually after. */
        wbinvd();
        advanceGuestRip(ctx);
        return;

    case EXIT_REASON_XSETBV:
        handleXsetbv(ctx);
        return;

    /* GETSEC (SMX) exits unconditionally too, whenever CR4.SMXE lets it
     * execute at all. CPUID reports no SMX, so #UD is the consistent
     * answer - and SMX would hand the guest a way to re-initialise the
     * platform underneath us. RSM belongs here for the same reason:
     * QEMU runs with -machine smm=off, so a guest trying to return
     * from SMM is confused, not legitimate. */
    /* VMCALL is how our own hooks call back into the host, marked by a
     * magic value in EBP. A VMCALL without that marker is a guest
     * probing or confused, and falls through to the #UD below with the
     * rest of the VMX instructions. */
    case EXIT_REASON_VMCALL:
        if (handleHypercall(ctx)) {
            advanceGuestRip(ctx);
            return;
        }
        injectException(EXCEPTION_VECTOR_UD, 0, 0);
        return;

    case EXIT_REASON_GETSEC:
    case EXIT_REASON_RSM:
    /* Every VMX instruction exits unconditionally. Since CPUID reports
     * no VMX, a guest executing one is either probing for a
     * hypervisor or confused - #UD is what it would get on a CPU
     * without VMX, so that's what it gets. */
    case EXIT_REASON_VMCLEAR:
    case EXIT_REASON_VMLAUNCH:
    case EXIT_REASON_VMPTRLD:
    case EXIT_REASON_VMPTRST:
    case EXIT_REASON_VMREAD:
    case EXIT_REASON_VMRESUME:
    case EXIT_REASON_VMWRITE:
    case EXIT_REASON_VMXOFF:
    case EXIT_REASON_VMXON:
    case EXIT_REASON_INVEPT:
    case EXIT_REASON_INVVPID:
    case EXIT_REASON_VMFUNC:
        injectException(EXCEPTION_VECTOR_UD, 0, 0);
        return;

    /* --- Control-gated exits --- */
    case EXIT_REASON_CR_ACCESS:
        handleCrAccess(ctx);
        return;

    case EXIT_REASON_RDMSR:
        handleRdmsr(ctx);
        return;

    case EXIT_REASON_WRMSR:
        handleWrmsr(ctx);
        return;

    case EXIT_REASON_INVLPG:
        /* Nothing for us to do - the guest's own TLB entries are the
         * CPU's business, and EPT isn't affected by INVLPG. Only
         * reachable if INVLPG exiting ever gets turned on. */
        advanceGuestRip(ctx);
        return;

    case EXIT_REASON_HLT:
        /* HLT exiting is off, so this shouldn't fire; skipping the
         * instruction turns the guest's idle loop into a spin, which
         * is wrong but survivable, and worth a line of output so it
         * doesn't go unnoticed. */
        logInfo("guest executed HLT - skipping it\n");
        advanceGuestRip(ctx);
        return;

    /* --- Fatal for now --- */
    case EXIT_REASON_EPT_VIOLATION:
        logError("EPT violation at guest-physical %lx (linear %lx, qualification %lx)\n",
                 ctx->guestPhysicalAddress, ctx->guestLinearAddress, ctx->exitQualification);
        logError("the EPT identity-maps the first 4GB, so this is either an "
                 "access above 4GB or a missing entry\n");
        dumpVmcsState();
        haltForever();
        return;

    case EXIT_REASON_EPT_MISCONFIG:
        logError("EPT misconfiguration at guest-physical %lx - a malformed entry "
                 "on the walk, not an access violation\n",
                 ctx->guestPhysicalAddress);
        dumpVmcsState();
        haltForever();
        return;

    case EXIT_REASON_TRIPLE_FAULT:
        reportFatalExit(ctx, "guest triple-faulted");
        return;

    default:
        reportFatalExit(ctx, "unhandled VM exit");
        return;
    }
}
