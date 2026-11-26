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
#include "disk.h"

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

/* ===================================================================
 * IO bitmap - two 4KB pages covering all 65536 port addresses.
 * A set bit means "VM exit on any I/O to this port"; a clear bit
 * passes through to QEMU's emulated hardware.
 *
 * We only intercept the three ports that can trigger a hardware CPU
 * reset, so we see the attempt and can log it before QEMU resets the
 * machine (which would restart VMBR entirely, hiding the root cause).
 *   0x64  i8042 keyboard controller - write 0xFE pulses reset line
 *   0x92  PS/2 System Control Port A - bit 0 (FAST_RESET) resets CPU
 *   0xCF9 System Reset Control Register - bit 2 triggers reset
 * Everything else (disk I/O, PIC, PCI, ...) is never intercepted, so
 * the guest reaches QEMU's emulated devices without any overhead.
 * =================================================================== */
#define RESET_PORT_KBC      0x64u   /* i8042 keyboard controller */
#define RESET_PORT_FAST_A20 0x92u   /* PS/2 System Control Port A */
#define RESET_PORT_CF9      0xCF9u  /* PCI system reset register */

static void setIoBit(uint8_t *bitmap, uint16_t port) {
    bitmap[port / 8] |= (uint8_t)(1u << (port % 8));
}

static int initializeIoBitmap(void) {
    uint8_t *bitmapA = allocateMemory(PAGE_SIZE); /* ports 0x0000-0x7FFF */
    uint8_t *bitmapB = allocateMemory(PAGE_SIZE); /* ports 0x8000-0xFFFF */
    if (!bitmapA || !bitmapB) {
        logError("couldn't allocate the IO bitmaps!!!\n");
        return 0;
    }
    memset(bitmapA, 0, PAGE_SIZE);
    memset(bitmapB, 0, PAGE_SIZE);

    setIoBit(bitmapA, RESET_PORT_KBC);
    setIoBit(bitmapA, RESET_PORT_FAST_A20);
    setIoBit(bitmapA, RESET_PORT_CF9);

    vmwrite(IO_BITMAP_A, (uint64_t)bitmapA);
    vmwrite(IO_BITMAP_B, (uint64_t)bitmapB);
    logInfo("IO bitmap: intercepting reset ports 0x64/0x92/0xCF9\n");
    return 1;
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
    uint32_t primaryProcBased = adjustControls(
        ACTIVATE_SECONDARY_CONTROLS | CPU_BASED_USE_MSR_BITMAPS | CPU_BASED_USE_IO_BITMAPS,
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

    if (!(primaryProcBased & CPU_BASED_USE_IO_BITMAPS)) {
        logError("CPU doesn't allow IO bitmaps - reset ports unprotected\n");
    }

    if (!initializeMsrBitmap()) {
        return 0;
    }
    if (primaryProcBased & CPU_BASED_USE_IO_BITMAPS) {
        if (!initializeIoBitmap()) {
            return 0;
        }
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

    /* Don't intercept guest exceptions - bootmgr handles its own #GP/#PF
     * via the IDT it just installed. We still get TRIPLE_FAULT via the
     * reserved exit reason if the guest's own IDT can't rescue it. */
    vmwrite(EXCEPTION_BITMAP, 0);
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
/* setupTss() is intentionally NOT called from here - it writes the GDT's
 * TSS descriptor and executes LTR, both of which are BSP-only work. The
 * GDT is shared across every CPU, and LTR on a TSS descriptor that's
 * already marked busy is #GP(0) per the SDM - so three APs racing into
 * this function and each running LTR on the already-busy descriptor the
 * BSP left behind triple-faulted the whole machine (no IDT yet). BSP
 * must call setupTss() itself before invoking this; APs reuse the GDT
 * slot the BSP configured and only populate their own VMCS HOST_TR_*
 * fields (initializeHostState below), which VM-exit loads without the
 * busy-bit check LTR enforces. */
static int initializeVmcs(uint64_t eptPointer) {
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
static void vmlaunch(uint8_t bootDrive) {
    uint64_t rflags;
    uint64_t drive = bootDrive;
    /* The guest's general-purpose registers are NOT part of the VMCS: on
     * the first entry the guest simply inherits whatever the physical
     * registers hold at vmlaunch. Loading RDX here therefore sets the
     * guest's initial RDX, whose low byte (DL) is the BIOS boot-drive
     * number every MBR reads to know which disk it booted from. If
     * vmlaunch succeeds control never returns here, so the guest starts
     * with DL already correct. */
    __asm__ volatile("mov %1, %%rdx\n\t"
                      "vmlaunch\n\t"
                      "pushfq\n\t"
                      "pop %0"
                      : "=r"(rflags)
                      : "r"(drive)
                      : "rdx", "cc", "memory");
    if (rflags & CARRY_FLAG_MASK) {
        logError("vmlaunch failed: VMfailInvalid\n");
        dumpVmcsState();
    } else if (rflags & ZERO_FLAG_MASK) {
        logError("vmlaunch failed: VMfailValid, error code %lu\n", vmread(VM_INSTRUCTION_ERROR));
        dumpVmcsState();
    }
}

/* --- Cross-CPU handoff for SMP bring-up ---
 * BSP builds the EPT once and publishes its EPTP here after all host
 * state is ready. APs wait for ready to flip to 1, then read eptPointer
 * and build their own per-CPU VMXON/VMCS pointing at the same EPT. */
static volatile uint64_t apSharedEptPointer = 0;
static volatile uint32_t apSharedReady = 0;

/* Where the AP guest stub ends up in low memory. 0x9F200 sits in the
 * HOOK_STUB_PROTECT range the guest E820 already marks reserved, so
 * Windows won't overwrite it. See hooks.c for the same protection. */
#define AP_GUEST_STUB_ADDR    0x9F200ull
#define AP_GUEST_STUB_SEGMENT ((uint16_t)(AP_GUEST_STUB_ADDR >> 4))

/* The AP guest stub itself: cli; hlt; jmp $-1 (loops back to hlt if
 * an NMI or anything else wakes HLT). Installed by BSP into the slot
 * above before releasing the APs. 4 bytes. */
static void installApGuestStub(void) {
    uint8_t *stub = (uint8_t *)AP_GUEST_STUB_ADDR;
    stub[0] = 0xFA;             /* cli */
    stub[1] = 0xF4;             /* hlt */
    stub[2] = 0xEB;             /* jmp */
    stub[3] = 0xFD;             /* -3 -> back to hlt */
}

/* Called by ap_main on each AP after long-mode entry. Mirrors BSP's
 * VMX bring-up in InitializeVmm, but points its guest state at the
 * tiny halt stub above instead of the MBR. */
void apBringUp(void) {
    /* Wait until BSP has published a usable EPTP. */
    while (!apSharedReady) {
        __asm__ volatile("pause");
    }
    uint64_t eptPointer = apSharedEptPointer;

    /* Per-CPU VMX enable (CR0/CR4 fixup + feature-control unlock). */
    if (!enableVmx()) {
        logError("AP: VMX disabled by firmware\n");
        for (;;) { __asm__ volatile("cli\n\thlt"); }
    }

    /* Per-CPU VMXON region. */
    void *vmxonRegion = allocateMemory(PAGE_SIZE);
    if (!vmxonRegion) {
        logError("AP: VMXON region alloc failed\n");
        for (;;) { __asm__ volatile("cli\n\thlt"); }
    }
    prepareVmxRegion(vmxonRegion);
    if (!vmxon((uint64_t)vmxonRegion)) {
        logError("AP: VMXON failed\n");
        for (;;) { __asm__ volatile("cli\n\thlt"); }
    }

    /* Per-CPU VMCS region. */
    void *vmcsRegion = allocateMemory(PAGE_SIZE);
    if (!vmcsRegion) {
        logError("AP: VMCS region alloc failed\n");
        for (;;) { __asm__ volatile("cli\n\thlt"); }
    }
    prepareVmxRegion(vmcsRegion);
    vmclear((uint64_t)vmcsRegion);
    if (!vmptrld((uint64_t)vmcsRegion)) {
        logError("AP: VMPTRLD failed\n");
        for (;;) { __asm__ volatile("cli\n\thlt"); }
    }

    /* Same VMCS layout as BSP, built against the shared EPT. setupTss()
     * is deliberately BSP-only (it writes the GDT's TSS slot and runs
     * LTR, which #GPs on a busy TSS); see the comment on initializeVmcs
     * for the full story. */
    if (!initializeVmcs(eptPointer)) {
        logError("AP: initializeVmcs failed\n");
        for (;;) { __asm__ volatile("cli\n\thlt"); }
    }

    /* Override the guest CS/RIP: this AP's non-root execution starts in
     * the tiny halt stub at AP_GUEST_STUB_ADDR (installed by BSP before
     * the release), not the BSP test guest's entry at CS:0700 / IP:0.
     *
     * TODO (phase C): the SDM wants an AP that hasn't received SIPI to
     * boot with GUEST_ACTIVITY_STATE = WAIT_FOR_SIPI so the guest OS
     * sees it exactly as hardware would. Leaving it ACTIVE for now
     * means the stub just runs its halt until the OS issues INIT -
     * which still exits to us and lets the INIT/SIPI handlers do the
     * right thing from there. */
    writeRealModeSegment(GUEST_CS_SELECTOR, GUEST_CS_BASE, GUEST_CS_LIMIT, GUEST_CS_AR_BYTES,
                         AP_GUEST_STUB_SEGMENT, REAL_MODE_CODE_AR);
    vmwrite(GUEST_RIP, 0);

    /* VMLAUNCH: never returns on success. bootDrive doesn't matter for
     * an AP guest - DL isn't an MBR entry on this path. */
    vmlaunch(0);
    logError("AP: vmlaunch returned - halting\n");
    for (;;) { __asm__ volatile("cli\n\thlt"); }
}

void InitializeVmm(void) {
    /* Find the boot disk and load its MBR to 0x7C00 BEFORE touching VMX.
     * disk_find_bootable_drive() reaches the disk through a BIOS int 0x13
     * round trip, which clears CR0.PE to drop to real mode - and VMX
     * operation forbids that (CR0.PE is forced to 1 once VMXON runs). So
     * all the disk work has to happen here, while we're still ordinary
     * host code. The MBR then sits untouched at 0x7C00 until launch. */
    uint8_t bootDrive = disk_find_bootable_drive();

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
    /* BSP-only: writes the GDT's TSS descriptor and executes LTR. APs
     * reuse the slot and must skip this - see the comment on
     * initializeVmcs for why. */
    setupTss();
    if (!initializeVmcs(eptPointer)) {
        return;
    }

    logSection("Guest");
    if (bootDrive != 0) {
        /* A bootable disk was found and its MBR is already sitting at
         * 0x7C00. Point the guest there instead of at the built-in test
         * payload: CS base 0, IP 0x7C00 (physical 0x7C00), which is the
         * entry point a PC MBR is loaded and jumped to by convention.
         * initializeGuestState() left CS=0x0700/IP=0 for the test guest;
         * override just those here. */
        writeRealModeSegment(GUEST_CS_SELECTOR, GUEST_CS_BASE, GUEST_CS_LIMIT, GUEST_CS_AR_BYTES,
                             0, REAL_MODE_CODE_AR);
        vmwrite(GUEST_RIP, MBR_LOAD_ADDRESS);
        logInfo("Booting disk 0x%x: guest entry at 0000:%x (DL=0x%x)\n",
                bootDrive, MBR_LOAD_ADDRESS, bootDrive);
    } else {
        /* No bootable disk - fall back to the built-in real-mode guest so
         * the VMX path is still exercised. CS/IP already point at it. */
        logInfo("No bootable disk - launching built-in test guest\n");
        copyRealModeGuest();
    }

    /* Everything the VMM owns has been allocated by now, so the
     * reserved ranges are final - install the E820 hook that hides
     * them from the guest. Must come after the guest is copied in (the
     * stub lands just above it) and before launch. */
    installGuestHooks();

    /* Telling the guest's E820 "reserved" isn't enough - Windows's VBR
     * and bootmgr both ignore the map in their early path and write
     * straight into low memory. Strip R/W/X on the EPT entries covering
     * every VMM range so a guest access there exits instead of silently
     * corrupting us. Must run after the last allocateMemory call, so the
     * footprint is final; we're still pre-vmlaunch so no INVEPT needed. */
    MemoryRange protect[VMM_MAX_RESERVED_RANGES];
    int protectCount = getVmmReservedRanges(protect, VMM_MAX_RESERVED_RANGES);
    for (int i = 0; i < protectCount; i++) {
        hideEptRange(eptPml4, protect[i].base, protect[i].end);
    }

    /* Install the AP guest stub and publish the shared EPTP. APs have
     * been waiting in apBringUp()'s spin on apSharedReady - they now
     * walk through the same VMX bring-up the BSP just did, land on
     * their own VMCS pointing at this EPT, and VMLAUNCH into the halt
     * stub above. Order matters: stub + hooks + EPT hiding must all be
     * in place before APs can launch, or their guests would see a VMM
     * that isn't quite finished setting itself up. */
    installApGuestStub();
    __atomic_store_n(&apSharedEptPointer, eptPointer, __ATOMIC_RELEASE);
    __atomic_store_n(&apSharedReady, 1u, __ATOMIC_RELEASE);

    logInfo("Guest is launching...\n");
    vmlaunch(bootDrive);
}
