/* vmx.c
 * VMXON/VMCS setup, ported from a reference implementation.
 *
 * A note on physical vs virtual addresses: VMXON/VMCLEAR/VMPTRLD all
 * take a *physical* address. setup_paging (paging.asm) identity-maps
 * the first 4GB, so virtual == physical everywhere allocateMemory can
 * hand out memory from right now, and the addresses below are used
 * directly without translation. That assumption breaks the moment
 * this kernel ever stops being identity-mapped.
 *
 * EPT is enabled (identity-mapped, see ept.c), so the guest's CR3 -
 * the host's own page tables - now holds guest-physical addresses,
 * which the EPT maps 1:1 onto the same host-physical memory.
 * Unrestricted guest is enabled too, and the guest is a real-mode
 * one (src/guest.asm): 16-bit code copied below 1MB and launched with
 * CR0.PE=0. VPID is not enabled.
 */
#include <stdint.h>
#include "vmx.h"
#include "cpu.h"
#include "memory.h"
#include "string.h"
#include "debug.h"
#include "vmcs.h"
#include "vmexit.h"
#include "ept.h"
#include "hooks.h"

/* CPUID.1:ECX.VMX[bit 5] */
static int checkForVmxSupport(void) {
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid"
                      : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                      : "a"(1));
    return (ecx & (1u << 5)) != 0;
}

/* Beyond CR4.VMXE, VMXON requires CR0 and CR4 to satisfy bit-exact
 * constraints the CPU reports via these MSRs: every bit set in
 * FIXED0 must be 1 in the register, and every bit *clear* in FIXED1
 * must be 0 in the register (FIXED1 is "the bits allowed to be 1").
 * Getting this wrong is a documented, easy-to-miss cause of VMXON
 * faulting outright with no VM-instruction-error to read (there's no
 * "current VMCS" yet at this point for VMREAD to report one from). */
static void fixupControlRegistersForVmx(void) {
    uint64_t cr0 = readCr0();
    cr0 |= readMsr(IA32_VMX_CR0_FIXED0_MSR);
    cr0 &= readMsr(IA32_VMX_CR0_FIXED1_MSR);
    writeCr0(cr0);

    uint64_t cr4 = readCr4() | CR4_VMXE;
    cr4 |= readMsr(IA32_VMX_CR4_FIXED0_MSR);
    cr4 &= readMsr(IA32_VMX_CR4_FIXED1_MSR);
    writeCr4(cr4);
}

/* IA32_FEATURE_CONTROL must have "VMX outside SMX" enabled - which
 * itself must happen before that MSR's lock bit gets set, since it's
 * write-once after that. If firmware already locked it without
 * enabling VMX, there's nothing left to do here but report failure. */
static int enableVmx(void) {
    fixupControlRegistersForVmx();

    uint64_t featureControl = readMsr(IA32_FEATURE_CONTROL_MSR);
    if (featureControl & FEATURE_CONTROL_LOCKED) {
        return (featureControl & FEATURE_CONTROL_VMX_OUTSIDE_SMX) != 0;
    }
    featureControl |= FEATURE_CONTROL_VMX_OUTSIDE_SMX | FEATURE_CONTROL_LOCKED;
    writeMsr(IA32_FEATURE_CONTROL_MSR, featureControl);
    return 1;
}

/* Both the VMXON region and a VMCS region start the same way: zeroed,
 * with the VMCS revision identifier (IA32_VMX_BASIC bits 0-30) in the
 * first 4 bytes and bit 31 clear - the rest is otherwise
 * implementation-defined, so zeroing it is just cleanliness. */
static void prepareVmxRegion(void *region) {
    memset(region, 0, PAGE_SIZE);
    uint32_t revisionId = (uint32_t)(readMsr(IA32_VMX_BASIC_MSR) & 0x7FFFFFFFu);
    *(uint32_t *)region = revisionId;
}

/* VMXON/VMCLEAR/VMPTRLD all report success/failure the same way (see
 * CARRY_FLAG_MASK/ZERO_FLAG_MASK in vmcs.h) - SETNA (set-if-CF-or-ZF)
 * reads exactly that in one
 * instruction. Each wrapper is a single asm block so nothing can
 * clobber the flags between the instruction and reading them. All
 * take a *physical* address (see the file-level comment). */
static int vmxon(uint64_t physAddr) {
    uint8_t fail;
    __asm__ volatile("vmxon %1\n\t"
                      "setna %0"
                      : "=q"(fail)
                      : "m"(physAddr)
                      : "cc");
    return !fail;
}

static int vmclear(uint64_t physAddr) {
    uint8_t fail;
    __asm__ volatile("vmclear %1\n\t"
                      "setna %0"
                      : "=q"(fail)
                      : "m"(physAddr)
                      : "cc");
    return !fail;
}

static int vmptrld(uint64_t physAddr) {
    uint8_t fail;
    __asm__ volatile("vmptrld %1\n\t"
                      "setna %0"
                      : "=q"(fail)
                      : "m"(physAddr)
                      : "cc");
    return !fail;
}

/* AT&T mechanically reverses Intel's operand order for every
 * 2-operand instruction. Intel's own operand lists put the
 * register-only operand (which can only ever hold a simple constant,
 * never point at memory) as the field encoding, and the flexible
 * r/m operand as the value/destination:
 *   VMWRITE field_reg, value_rm   ->  AT&T: vmwrite value, field
 *   VMREAD  value_rm, field_reg   ->  AT&T: vmread  field, value
 * Getting this backwards silently writes/reads the wrong VMCS field
 * with no compile-time or assemble-time signal that anything's wrong. */
void vmwrite(uint64_t field, uint64_t value) {
    uint8_t fail;
    __asm__ volatile("vmwrite %2, %1\n\t"
                      "setna %0"
                      : "=q"(fail)
                      : "r"(field), "r"(value)
                      : "cc");
    if (fail) {
        logError("VMWRITE failed for field %lx\n", field);
    }
}

uint64_t vmread(uint64_t field) {
    uint64_t value;
    __asm__ volatile("vmread %1, %0"
                      : "=r"(value)
                      : "r"(field));
    return value;
}

/* A desired-bits value combined with a VMX capability MSR: bits 0-31
 * of the MSR are "must be 1" (allowed0), bits 32-63 are "may be 1"
 * (allowed1) - the actual value to write is
 * (desired | allowed0) & allowed1, forcing every required bit on and
 * clearing every disallowed one regardless of what was asked for. */
static uint32_t adjustControls(uint32_t desired, uint32_t capabilityMsr) {
    uint64_t capability = readMsr(capabilityMsr);
    uint32_t allowed0 = (uint32_t)capability;
    uint32_t allowed1 = (uint32_t)(capability >> 32);
    return (desired | allowed0) & allowed1;
}

/* IA32_VMX_BASIC bit 55: if set, the "true" capability MSRs must be
 * used instead of the original ones (the original ones can report
 * some required bits as "must be 1" when they're actually optional -
 * the true variants correct that). */
static int usesTrueControls(void) {
    return (readMsr(IA32_VMX_BASIC_MSR) & (1ull << 55)) != 0;
}

/* ===================================================================
 * TSS - VMX requires TR (both guest and host) to reference a valid,
 * loaded TSS descriptor. Nothing needed one before this.
 * =================================================================== */
#define TSS_SIZE 104u
/* Position of .tss in gdt.asm's table: null=0, code64=8, data64=0x10,
 * code32=0x18, data32=0x20, code16=0x28, data16=0x30, tss=0x38.
 * Hardcoded rather than imported via extern - it's a fixed constant
 * entirely determined by a table this file doesn't otherwise touch,
 * and importing an absolute NASM symbol's *value* into C needs an
 * awkward extern-array-decay trick for no benefit here. */
#define GDT_TSS_SEL 0x38u

static uint8_t tss[TSS_SIZE] __attribute__((aligned(16)));

extern void set_tss_base(uint64_t tssAddress);

static void setupTss(void) {
    memset(tss, 0, TSS_SIZE);
    /* I/O Map Base Address (last 2 bytes) = TSS_SIZE, i.e. pointing
     * past the segment limit - the architectural way to say "no I/O
     * permission bitmap present". */
    tss[TSS_SIZE - 2] = (uint8_t)(TSS_SIZE & 0xFFu);
    tss[TSS_SIZE - 1] = (uint8_t)((TSS_SIZE >> 8) & 0xFFu);

    set_tss_base((uint64_t)tss);
    loadTR(GDT_TSS_SEL);
}

/* Segment access-rights bits are in vmcs.h - the exit handler needs
 * them too, to tell a 64-bit guest from a 32-bit one. */

/* Clears a selector's RPL (bits 0-1) and TI (bit 2) - the guest/host
 * start on the same flat, ring-0, GDT-based selectors the kernel
 * itself is already running on. */
#define ACCESS_RIGHTS_MASK 0xFFF8u

#define STACK_SIZE 0x8000ull /* 32KB host stack for VM-exit handling */

/* Where the real-mode guest runs: CS=0x0700, IP=0, i.e. physical
 * 0x7000. Free conventional memory - E820 region 0 is usable RAM, and
 * allocateMemory only hands out memory from the top of the big region
 * above 1MB, never from here. Its stack sits right below it, growing
 * down from SS=0:SP=0x7000. */
#define REAL_MODE_GUEST_SEGMENT 0x0700u
#define REAL_MODE_GUEST_ADDRESS ((uint64_t)REAL_MODE_GUEST_SEGMENT << 4)
#define REAL_MODE_STACK_POINTER 0x7000u

/* Real-mode segments: 64KB limit (bits 11:0 all 1 and 31:20 all 0, so
 * G=0 is the consistent choice), D/B=0 (16-bit), L=0. Code is type 0xB
 * (accessed, readable code), data is type 3 (accessed, writable data). */
#define REAL_MODE_LIMIT   0xFFFFu
#define REAL_MODE_CODE_AR (SEG_A | SEG_RW | SEG_E | SEG_S | SEG_P)
#define REAL_MODE_DATA_AR (SEG_A | SEG_RW | SEG_S | SEG_P)

/* The real-mode guest's code, from src/guest.asm. */
extern uint8_t real_mode_guest_start[];
extern uint8_t real_mode_guest_end[];

static void copyRealModeGuest(void) {
    uint64_t size = (uint64_t)(real_mode_guest_end - real_mode_guest_start);
    memcpy((void *)REAL_MODE_GUEST_ADDRESS, real_mode_guest_start, size);
    logInfo("Real-mode guest copied to %lx (%lu bytes)\n", REAL_MODE_GUEST_ADDRESS, size);
}

/* Real mode: a segment's base is always selector * 16. */
static void writeRealModeSegment(uint64_t selectorField, uint64_t baseField, uint64_t limitField,
                                 uint64_t accessRightsField, uint16_t selector, uint32_t accessRights) {
    vmwrite(selectorField, selector);
    vmwrite(baseField, (uint64_t)selector << 4);
    vmwrite(limitField, REAL_MODE_LIMIT);
    vmwrite(accessRightsField, accessRights);
}

static void initializeGuestState(void) {
    /* Ask for nothing at all in CR0/CR4 and let writeGuestCr0/Cr4 add
     * back whatever VMX insists on: PE=0 and PG=0 (real mode, which
     * only unrestricted guest permits), and no PAE/PSE.
     *
     * Deliberately NOT derived from the host's own CR0/CR4 any more.
     * The host runs with PAE on and paging enabled; handing those to a
     * guest OS that is about to build its own page tables and turn
     * paging on itself means it enables PG with a PAE setting it never
     * chose - and a 32-bit bootloader expecting plain two-level paging
     * then walks tables in the wrong format. A guest's control
     * registers should start where a CPU out of reset leaves them,
     * not where the host happens to be.
     *
     * EFER goes first because writeGuestCr0 reconciles CR0.PG against
     * it, and reading a VMCS field that was never written isn't
     * architecturally defined. LME=0, LMA=0: loaded on entry because
     * of VM_ENTRY_LOAD_IA32_EFER, and LMA must agree with the
     * IA32e-mode-guest entry control, which starts clear. */
    vmwrite(GUEST_IA32_EFER, 0);
    writeGuestCr0(CR0_ET); /* ET is set on every CPU since the 486 */
    writeGuestCr4(0);
    /* Unused with paging off, but must still be a valid address. */
    vmwrite(GUEST_CR3, 0);
    vmwrite(GUEST_DR7, readDr7());
    vmwrite(GUEST_RSP, REAL_MODE_STACK_POINTER);
    vmwrite(GUEST_RIP, 0); /* offset within CS - see REAL_MODE_GUEST_SEGMENT */
    vmwrite(GUEST_RFLAGS, 0x2); /* only the always-1 bit: IF=0, DF=0, VM=0 */

    writeRealModeSegment(GUEST_CS_SELECTOR, GUEST_CS_BASE, GUEST_CS_LIMIT, GUEST_CS_AR_BYTES,
                         REAL_MODE_GUEST_SEGMENT, REAL_MODE_CODE_AR);
    writeRealModeSegment(GUEST_SS_SELECTOR, GUEST_SS_BASE, GUEST_SS_LIMIT, GUEST_SS_AR_BYTES,
                         0, REAL_MODE_DATA_AR);
    writeRealModeSegment(GUEST_DS_SELECTOR, GUEST_DS_BASE, GUEST_DS_LIMIT, GUEST_DS_AR_BYTES,
                         0, REAL_MODE_DATA_AR);
    writeRealModeSegment(GUEST_ES_SELECTOR, GUEST_ES_BASE, GUEST_ES_LIMIT, GUEST_ES_AR_BYTES,
                         0, REAL_MODE_DATA_AR);
    writeRealModeSegment(GUEST_FS_SELECTOR, GUEST_FS_BASE, GUEST_FS_LIMIT, GUEST_FS_AR_BYTES,
                         0, REAL_MODE_DATA_AR);
    writeRealModeSegment(GUEST_GS_SELECTOR, GUEST_GS_BASE, GUEST_GS_LIMIT, GUEST_GS_AR_BYTES,
                         0, REAL_MODE_DATA_AR);

    /* No LDT in use - mark unusable rather than pointing at a bogus descriptor. */
    vmwrite(GUEST_LDTR_SELECTOR, 0);
    vmwrite(GUEST_LDTR_BASE, 0);
    vmwrite(GUEST_LDTR_LIMIT, 0);
    vmwrite(GUEST_LDTR_AR_BYTES, SEG_UNUSABLE);

    /* TR: a busy TSS as a SYSTEM descriptor - SEG_S deliberately
     * omitted (SEG_A|SEG_RW|SEG_E = bits 0,1,3 = type 0xB). Without
     * IA32E_MODE_GUEST, VM-entry accepts type 3 or 0xB here, so the
     * same value still works for the real-mode guest, which never
     * uses TR anyway. G must also be 0: VM-entry requires G=0 whenever any
     * of limit bits 11:0 is 0, and this limit is the real TSS size
     * (0x67). The reference got away with G=1 only because it used a
     * 0xFFFFFFFF limit for TR - mixing its G=1 with a real 0x67 limit
     * fails VM-entry with reason 0x80000021. */
    vmwrite(GUEST_TR_SELECTOR, GDT_TSS_SEL);
    vmwrite(GUEST_TR_BASE, (uint64_t)tss);
    vmwrite(GUEST_TR_LIMIT, TSS_SIZE - 1);
    vmwrite(GUEST_TR_AR_BYTES, SEG_A | SEG_RW | SEG_E | SEG_P);

    /* Real mode ignores GDTR, but the field must still be valid. */
    vmwrite(GUEST_GDTR_BASE, readGdtrBase());
    vmwrite(GUEST_GDTR_LIMIT, readGdtrLimit());
    /* Real-mode interrupt vector table: 256 entries * 4 bytes at 0. */
    vmwrite(GUEST_IDTR_BASE, 0);
    vmwrite(GUEST_IDTR_LIMIT, 0x3FF);

    vmwrite(GUEST_IA32_DEBUGCTL, 0);
    vmwrite(GUEST_SYSENTER_CS, 0);
    vmwrite(GUEST_SYSENTER_ESP, 0);
    vmwrite(GUEST_SYSENTER_EIP, 0);
    vmwrite(GUEST_ACTIVITY_STATE, 0);
    vmwrite(GUEST_INTERRUPTIBILITY_INFO, 0);
    vmwrite(GUEST_PENDING_DBG_EXCEPTIONS, 0);

    /* Required to be all-1s when not using VMCS shadowing - VM-entry
     * checks this, and it's an easy field to forget entirely. */
    vmwrite(VMCS_LINK_POINTER, 0xFFFFFFFFFFFFFFFFull);
}

static void initializeHostState(void) {
    vmwrite(HOST_CR0, readCr0());
    vmwrite(HOST_CR3, readCr3());
    vmwrite(HOST_CR4, readCr4());
    /* The asm stub, not the C handler: a VM exit leaves every GPR
     * holding the guest's value, and compiled code would destroy them
     * before anything could save them (see vmexit_entry.asm). */
    vmwrite(HOST_RIP, (uint64_t)vmexit_entry);

    void *hostStack = allocateMemory(STACK_SIZE);
    vmwrite(HOST_RSP, (uint64_t)hostStack + STACK_SIZE); /* stack grows down - start at the top */

    vmwrite(HOST_CS_SELECTOR, readCS() & ACCESS_RIGHTS_MASK);
    vmwrite(HOST_SS_SELECTOR, readSS() & ACCESS_RIGHTS_MASK);
    vmwrite(HOST_DS_SELECTOR, readDS() & ACCESS_RIGHTS_MASK);
    vmwrite(HOST_ES_SELECTOR, readES() & ACCESS_RIGHTS_MASK);
    vmwrite(HOST_FS_SELECTOR, readFS() & ACCESS_RIGHTS_MASK);
    vmwrite(HOST_GS_SELECTOR, readGS() & ACCESS_RIGHTS_MASK);
    vmwrite(HOST_TR_SELECTOR, GDT_TSS_SEL);

    vmwrite(HOST_FS_BASE, readMsr(IA32_FS_BASE_MSR));
    vmwrite(HOST_GS_BASE, readMsr(IA32_GS_BASE_MSR));
    vmwrite(HOST_TR_BASE, (uint64_t)tss);
    vmwrite(HOST_GDTR_BASE, readGdtrBase());
    vmwrite(HOST_IDTR_BASE, readIdtrBase());

    vmwrite(HOST_IA32_SYSENTER_CS, 0);
    vmwrite(HOST_IA32_SYSENTER_ESP, 0);
    vmwrite(HOST_IA32_SYSENTER_EIP, 0);
    vmwrite(HOST_IA32_EFER, readMsr(IA32_EFER_MSR));
}

/* ===================================================================
 * MSR bitmap - four 1KB bit arrays in one page: read-low, read-high,
 * write-low, write-high. A set bit means "VM exit on this access"; a
 * clear bit lets the guest reach the real MSR with no exit at all.
 *
 * Without a bitmap *every* RDMSR/WRMSR exits, and an OS does thousands
 * of them before it finishes booting. So the page starts all-zero and
 * only the handful of MSRs we actually have to lie about get marked -
 * see handleRdmsr/handleWrmsr in vmexit.c.
 * =================================================================== */
#define MSR_BITMAP_READ_LOW   0u
#define MSR_BITMAP_READ_HIGH  1024u
#define MSR_BITMAP_WRITE_LOW  2048u
#define MSR_BITMAP_WRITE_HIGH 3072u

#define MSR_LOW_RANGE_END    0x2000u
#define MSR_HIGH_RANGE_START 0xC0000000u
#define MSR_HIGH_RANGE_END   0xC0002000u

static void interceptMsr(uint8_t *bitmap, uint32_t msr) {
    uint32_t index;
    uint32_t readBase;
    uint32_t writeBase;

    if (msr < MSR_LOW_RANGE_END) {
        index = msr;
        readBase = MSR_BITMAP_READ_LOW;
        writeBase = MSR_BITMAP_WRITE_LOW;
    } else if (msr >= MSR_HIGH_RANGE_START && msr < MSR_HIGH_RANGE_END) {
        index = msr - MSR_HIGH_RANGE_START;
        readBase = MSR_BITMAP_READ_HIGH;
        writeBase = MSR_BITMAP_WRITE_HIGH;
    } else {
        /* An MSR outside both ranges exits unconditionally whatever
         * the bitmap says, so there's no bit to set. */
        return;
    }

    bitmap[readBase + index / 8] |= (uint8_t)(1u << (index % 8));
    bitmap[writeBase + index / 8] |= (uint8_t)(1u << (index % 8));
}

static int initializeMsrBitmap(void) {
    uint8_t *bitmap = allocateMemory(PAGE_SIZE);
    if (!bitmap) {
        logError("couldn't allocate the MSR bitmap!!!\n");
        return 0;
    }
    memset(bitmap, 0, PAGE_SIZE);

    /* EFER: the guest's copy lives in the VMCS, and a write to it is
     * how a guest announces it's about to enable long mode. */
    interceptMsr(bitmap, IA32_EFER_MSR);
    /* IA32_FEATURE_CONTROL: reported locked with VMX disabled, so the
     * guest doesn't try to run VMX of its own. */
    interceptMsr(bitmap, IA32_FEATURE_CONTROL_MSR);

    vmwrite(MSR_BITMAP, (uint64_t)bitmap);
    return 1;
}

static int initializeControls(uint64_t eptPointer) {
    int trueControls = usesTrueControls();

    /* External-interrupt exiting stays off deliberately: device
     * interrupts go straight to the guest, which is what we want for
     * a guest that owns the hardware - and it means the host needs no
     * IDT of its own. */
    uint32_t pinBased = adjustControls(0,
        trueControls ? IA32_VMX_TRUE_PINBASED_CTLS_MSR : IA32_VMX_PINBASED_CTLS_MSR);
    vmwrite(PIN_BASED_VM_EXEC_CONTROL, pinBased);

    /* adjustControls silently drops any requested bit the CPU doesn't
     * allow, so check the bits EPT depends on actually survived. */
    uint32_t primaryProcBased = adjustControls(ACTIVATE_SECONDARY_CONTROLS | CPU_BASED_USE_MSR_BITMAPS,
        trueControls ? IA32_VMX_TRUE_PROCBASED_CTLS_MSR : IA32_VMX_PROCBASED_CTLS_MSR);
    if (!(primaryProcBased & ACTIVATE_SECONDARY_CONTROLS)) {
        logError("CPU doesn't allow secondary controls - can't enable EPT!!!\n");
        return 0;
    }
    if (!(primaryProcBased & CPU_BASED_USE_MSR_BITMAPS)) {
        /* Not fatal: without a bitmap every MSR access exits, which is
         * slow but still correct, since the handler passes through
         * everything it doesn't special-case. */
        logError("CPU doesn't allow MSR bitmaps - every MSR access will exit\n");
    }
    vmwrite(CPU_BASED_VM_EXEC_CONTROL, primaryProcBased);

    /* RDTSCP/INVPCID/XSAVES are requested because without their enable
     * bits the guest gets #UD on instructions a modern OS simply uses -
     * a failure that looks nothing like a missing control bit. */
    uint32_t secondaryProcBased = adjustControls(
        SECONDARY_ENABLE_EPT | SECONDARY_UNRESTRICTED_GUEST |
        SECONDARY_ENABLE_RDTSCP | SECONDARY_ENABLE_INVPCID | SECONDARY_ENABLE_XSAVES,
        IA32_VMX_PROCBASED_CTLS2_MSR);
    if (!(secondaryProcBased & SECONDARY_ENABLE_EPT)) {
        logError("CPU doesn't allow EPT!!!\n");
        return 0;
    }
    if (!(secondaryProcBased & SECONDARY_UNRESTRICTED_GUEST)) {
        logError("CPU doesn't allow unrestricted guest!!!\n");
        return 0;
    }
    if (!(secondaryProcBased & SECONDARY_ENABLE_RDTSCP)) {
        logError("CPU doesn't allow RDTSCP in the guest - it will #UD there\n");
    }
    if (!(secondaryProcBased & SECONDARY_ENABLE_INVPCID)) {
        logError("CPU doesn't allow INVPCID in the guest - it will #UD there\n");
    }
    if (!(secondaryProcBased & SECONDARY_ENABLE_XSAVES)) {
        logError("CPU doesn't allow XSAVES in the guest - it will #UD there\n");
    }
    vmwrite(SECONDARY_VM_EXEC_CONTROL, secondaryProcBased);
    vmwrite(EPT_POINTER, eptPointer);

    if (!initializeMsrBitmap()) {
        return 0;
    }

    /* HOST_ADDR_SPACE_SIZE is required: the host is running in 64-bit
     * mode, so VM-exit must return to 64-bit mode too. The two EFER
     * controls are what make the guest's EFER readable on exit (see
     * vmcs.h) - without the save side, the exit handler has no way to
     * find out whether the guest has set LME. */
    uint32_t exitControls = adjustControls(
        VM_EXIT_HOST_ADDR_SPACE_SIZE | VM_EXIT_SAVE_IA32_EFER | VM_EXIT_LOAD_IA32_EFER,
        trueControls ? IA32_VMX_TRUE_EXIT_CTLS_MSR : IA32_VMX_EXIT_CTLS_MSR);
    if (!(exitControls & VM_EXIT_SAVE_IA32_EFER)) {
        logError("CPU doesn't allow saving the guest's EFER on VM-exit!!!\n");
        return 0;
    }
    vmwrite(VM_EXIT_CONTROLS, exitControls);

    /* IA32E_MODE_GUEST is deliberately not requested here - the guest
     * starts in real mode, and from then on it's maintained by
     * syncLongModeEntryControl (vmexit.c) as the guest switches modes. */
    uint32_t entryControls = adjustControls(VM_ENTRY_LOAD_IA32_EFER,
        trueControls ? IA32_VMX_TRUE_ENTRY_CTLS_MSR : IA32_VMX_ENTRY_CTLS_MSR);
    if (!(entryControls & VM_ENTRY_LOAD_IA32_EFER)) {
        logError("CPU doesn't allow loading the guest's EFER on VM-entry!!!\n");
        return 0;
    }
    vmwrite(VM_ENTRY_CONTROLS, entryControls);

    /* Which CR0/CR4 bits the host owns, i.e. which guest writes exit:
     *   CR0.PG  - the only way to learn the guest is enabling paging,
     *             and therefore possibly long mode
     *   CR0.PE  - the real-mode -> protected-mode switch, so the whole
     *             sequence funnels through one handler
     *   CR0.NE  - required to be 1 by IA32_VMX_CR0_FIXED0; owning it
     *             means a guest write can't leave GUEST_CR0 in a state
     *             that fails the *next* VM entry
     *   CR4.VMXE - forced on in GUEST_CR4, so it has to be owned (and
     *             hidden by the read shadow) rather than visible
     * Everything else is the guest's, written through with no exit. */
    vmwrite(CR0_GUEST_HOST_MASK, CR0_PE | CR0_PG | CR0_NE);
    vmwrite(CR4_GUEST_HOST_MASK, CR4_VMXE);
    vmwrite(CR3_TARGET_COUNT, 0);

    vmwrite(EXCEPTION_BITMAP, 0); /* don't intercept any exceptions yet */
    vmwrite(VM_EXIT_MSR_STORE_COUNT, 0);
    vmwrite(VM_EXIT_MSR_LOAD_COUNT, 0);
    vmwrite(VM_ENTRY_MSR_LOAD_COUNT, 0);
    vmwrite(VM_ENTRY_INTR_INFO_FIELD, 0);
    return 1;
}

/* Controls go in before guest state, not after: the guest's CR0 now
 * goes through writeGuestCr0, which reconciles the IA32e-mode-guest
 * entry control against it, so VM_ENTRY_CONTROLS has to hold a real
 * value by then. */
static int initializeVmcs(uint64_t eptPointer) {
    setupTss();
    if (!initializeControls(eptPointer)) {
        return 0;
    }
    initializeGuestState();
    initializeHostState();
    logInfo("VMCS fields initialized, EPT and unrestricted guest enabled\n");
    return 1;
}

/* On success, vmlaunch transfers control straight to the guest
 * (the real-mode code at REAL_MODE_GUEST_ADDRESS) and never returns
 * here at all - the pushfq/pop below only
 * executes if vmlaunch itself failed synchronously. CF=1 means
 * VMfailInvalid (no valid current VMCS - shouldn't happen, we just
 * VMPTRLD'd one); ZF=1 means VMfailValid, with a specific reason
 * readable from VM_INSTRUCTION_ERROR now that there IS a current
 * VMCS to read it from (unlike VMXON's failure mode earlier, which
 * had no such field available yet). */
static void vmlaunch(void) {
    uint64_t rflags;
    __asm__ volatile("vmlaunch\n\t"
                      "pushfq\n\t"
                      "pop %0"
                      : "=r"(rflags));
    if (rflags & CARRY_FLAG_MASK) {
        logError("vmlaunch failed: VMfailInvalid\n");
        dumpVmcsState();
    } else if (rflags & ZERO_FLAG_MASK) {
        logError("vmlaunch failed: VMfailValid, error code %lu\n", vmread(VM_INSTRUCTION_ERROR));
        dumpVmcsState();
    }
}

void InitializeVmm(void) {
    logSection("VMX");

    if (!checkForVmxSupport()) {
        logError("VMX is not supported!!!\n");
        return;
    }
    logInfo("VMX is supported\n");

    if (!enableVmx()) {
        logError("VMX is disabled and locked by firmware (IA32_FEATURE_CONTROL) - cannot proceed!!!\n");
        return;
    }

    void *vmxonRegion = allocateMemory(PAGE_SIZE);
    prepareVmxRegion(vmxonRegion);
    if (vmxon((uint64_t)vmxonRegion)) {
        logInfo("Successfully executed VMXON\n");
    } else {
        logError("Error while executing VMXON!!!\n");
        return;
    }

    void *vmcsRegion = allocateMemory(PAGE_SIZE);
    prepareVmxRegion(vmcsRegion);
    vmclear((uint64_t)vmcsRegion);
    if (vmptrld((uint64_t)vmcsRegion)) {
        logInfo("Successfully loaded VMCS region\n");
    } else {
        logError("Error while loading VMCS region!!!\n");
        return;
    }

    logSection("EPT");
    EptTableEntry *eptPml4 = initializeEpt();
    if (!eptPml4) {
        return;
    }
    uint64_t eptPointer = buildEptPointer(eptPml4);
    if (!eptPointer) {
        return;
    }

    /* No INVEPT needed here: this EPTP has never been used, so there
     * are no cached translations for it yet. Once EPT entries get
     * changed after launch (e.g. to protect host memory), each change
     * needs an INVEPT. */
    logSection("VMCS");
    if (!initializeVmcs(eptPointer)) {
        return;
    }

    logSection("Guest");
    copyRealModeGuest();

    /* Everything the VMM owns has been allocated by now, so the
     * reserved ranges are final - install the E820 hook that hides
     * them from the guest. Must come after the guest is copied in (the
     * stub lands just above it) and before launch. */
    installGuestHooks();

    logInfo("Guest is launching...\n");
    vmlaunch();
}
