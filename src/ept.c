/* ept.c
 * Builds an identity-mapped EPT: guest-physical address X translates
 * to host-physical address X. 4KB pages throughout (no 2MB/1GB large
 * pages) so that later steps can set a memory type and protection per
 * 4KB page - MTRR ranges and the host's own structures don't line up
 * with 2MB boundaries.
 *
 * Table pointers from allocateMemory are used directly as physical
 * addresses - valid only because the host is identity-mapped (see the
 * same note at the top of vmx.c).
 */
#include <stdint.h>
#include "ept.h"
#include "memory.h"
#include "debug.h"
#include "cpu.h"

/* Same 4GB paging.asm maps for the host (MEM_SIZE_GB there - keep the
 * two in sync). That covers RAM and the MMIO below 4GB, e.g. the Local
 * APIC at 0xFEE00000, so the guest can reach everything the host can. */
#define EPT_MAPPED_GB 4ull
#define ENTRIES_PER_TABLE 512ull

/* ===================================================================
 * Memory types from the MTRRs (SDM Vol 3A 12.11). EPT's memoryType
 * field uses the same encoding as the MTRRs, so values copy straight
 * across.
 * =================================================================== */
#define IA32_MTRRCAP_MSR        0xFEu
#define IA32_MTRR_DEF_TYPE_MSR  0x2FFu
#define IA32_MTRR_PHYSBASE0_MSR 0x200u /* PHYSBASEn = 0x200+2n, PHYSMASKn = 0x201+2n */

#define MTRRCAP_VCNT_MASK   0xFFull
#define MTRRCAP_FIX         (1ull << 8)
#define MTRR_DEF_TYPE_MASK  0xFFull
#define MTRR_DEF_TYPE_FE    (1ull << 10)
#define MTRR_DEF_TYPE_E     (1ull << 11)
#define MTRR_PHYSMASK_VALID (1ull << 11)

#define MEMORY_TYPE_UC 0
#define MEMORY_TYPE_WC 1
#define MEMORY_TYPE_WT 4
#define MEMORY_TYPE_WP 5
#define MEMORY_TYPE_WB 6
/* Not a valid memory type - marks pages no variable range has matched
 * yet, so the default type can be applied to exactly those afterwards. */
#define MEMORY_TYPE_UNMATCHED 7

/* SDM Table 12-9: each fixed-range MSR holds 8 one-byte types, byte i
 * covering [start + i*step, start + (i+1)*step). */
static const struct {
    uint32_t msr;
    uint64_t start;
    uint64_t step;
} fixedRanges[] = {
    { 0x250, 0x00000, 0x10000 }, /* IA32_MTRR_FIX64K_00000 */
    { 0x258, 0x80000, 0x4000 },  /* IA32_MTRR_FIX16K_80000 */
    { 0x259, 0xA0000, 0x4000 },  /* IA32_MTRR_FIX16K_A0000 */
    { 0x268, 0xC0000, 0x1000 },  /* IA32_MTRR_FIX4K_C0000 */
    { 0x269, 0xC8000, 0x1000 },  /* IA32_MTRR_FIX4K_C8000 */
    { 0x26A, 0xD0000, 0x1000 },  /* IA32_MTRR_FIX4K_D0000 */
    { 0x26B, 0xD8000, 0x1000 },  /* IA32_MTRR_FIX4K_D8000 */
    { 0x26C, 0xE0000, 0x1000 },  /* IA32_MTRR_FIX4K_E0000 */
    { 0x26D, 0xE8000, 0x1000 },  /* IA32_MTRR_FIX4K_E8000 */
    { 0x26E, 0xF0000, 0x1000 },  /* IA32_MTRR_FIX4K_F0000 */
    { 0x26F, 0xF8000, 0x1000 },  /* IA32_MTRR_FIX4K_F8000 */
};

static const char *memoryTypeName(uint64_t type) {
    switch (type) {
    case MEMORY_TYPE_UC: return "UC";
    case MEMORY_TYPE_WC: return "WC";
    case MEMORY_TYPE_WT: return "WT";
    case MEMORY_TYPE_WP: return "WP";
    case MEMORY_TYPE_WB: return "WB";
    default:             return "??";
    }
}

/* Overlapping variable ranges (SDM 12.11.4.1): UC wins over anything,
 * WT+WB gives WT, and any other mix is undefined - UC is the safe pick. */
static uint64_t combineVariableTypes(uint64_t current, uint64_t incoming) {
    if (current == MEMORY_TYPE_UNMATCHED || current == incoming) {
        return incoming;
    }
    if (current == MEMORY_TYPE_UC || incoming == MEMORY_TYPE_UC) {
        return MEMORY_TYPE_UC;
    }
    if ((current == MEMORY_TYPE_WT && incoming == MEMORY_TYPE_WB) ||
        (current == MEMORY_TYPE_WB && incoming == MEMORY_TYPE_WT)) {
        return MEMORY_TYPE_WT;
    }
    return MEMORY_TYPE_UC;
}

/* PHYSBASE/PHYSMASK only hold address bits below MAXPHYADDR. */
static uint64_t physicalAddressMask(void) {
    uint32_t eax, ebx, ecx, edx;
    uint32_t width = 36; /* architectural fallback if the leaf is missing */
    cpuid(0x80000000u, &eax, &ebx, &ecx, &edx);
    if (eax >= 0x80000008u) {
        cpuid(0x80000008u, &eax, &ebx, &ecx, &edx);
        width = eax & 0xFF;
    }
    return ((1ull << width) - 1) & ~0xFFFull;
}

static void applyMtrrMemoryTypes(EptPageEntry *pt, uint64_t pageCount) {
    uint32_t eax, ebx, ecx, edx;
    cpuid(1, &eax, &ebx, &ecx, &edx);
    if (!(edx & (1u << 12))) {
        logError("MTRRs not supported - all EPT memory left UC\n");
        return;
    }

    uint64_t defType = readMsr(IA32_MTRR_DEF_TYPE_MSR);
    if (!(defType & MTRR_DEF_TYPE_E)) {
        logInfo("MTRRs disabled - all EPT memory left UC\n");
        return;
    }
    uint64_t defaultType = defType & MTRR_DEF_TYPE_MASK;
    uint64_t capability = readMsr(IA32_MTRRCAP_MSR);
    uint64_t variableCount = capability & MTRRCAP_VCNT_MASK;
    int fixedEnabled = (defType & MTRR_DEF_TYPE_FE) && (capability & MTRRCAP_FIX);
    uint64_t addressMask = physicalAddressMask();

    logInfo("MTRRs: default type %s, %lu variable ranges, fixed ranges %s\n",
            memoryTypeName(defaultType), variableCount, fixedEnabled ? "on" : "off");

    /* 1. Variable ranges. Pages start UNMATCHED; overlaps are resolved
     *    with the SDM precedence rules instead of last-one-wins. */
    for (uint64_t page = 0; page < pageCount; page++) {
        pt[page].fields.memoryType = MEMORY_TYPE_UNMATCHED;
    }
    for (uint64_t n = 0; n < variableCount; n++) {
        uint64_t base = readMsr(IA32_MTRR_PHYSBASE0_MSR + 2 * n);
        uint64_t mask = readMsr(IA32_MTRR_PHYSBASE0_MSR + 2 * n + 1);
        if (!(mask & MTRR_PHYSMASK_VALID)) {
            continue;
        }
        uint64_t type = base & MTRR_DEF_TYPE_MASK;
        uint64_t rangeMask = mask & addressMask;
        uint64_t rangeBase = base & rangeMask;
        uint64_t rangeSize = (~rangeMask & addressMask) + PAGE_SIZE;
        logInfo("variable range: %lx-%lx %s\n",
                rangeBase, rangeBase + rangeSize - 1, memoryTypeName(type));

        /* Membership test per page rather than a computed length, so
         * a non-contiguous mask is still handled correctly. Pages past
         * the EPT's 4GB are simply never visited. */
        for (uint64_t page = 0; page < pageCount; page++) {
            if (((page * PAGE_SIZE) & rangeMask) == rangeBase) {
                pt[page].fields.memoryType =
                    combineVariableTypes(pt[page].fields.memoryType, type);
            }
        }
    }

    /* 2. Everything no variable range covered gets the default type. */
    for (uint64_t page = 0; page < pageCount; page++) {
        if (pt[page].fields.memoryType == MEMORY_TYPE_UNMATCHED) {
            pt[page].fields.memoryType = defaultType;
        }
    }

    /* 3. Below 1MB the fixed ranges override everything else. */
    if (fixedEnabled) {
        for (uint64_t r = 0; r < sizeof(fixedRanges) / sizeof(fixedRanges[0]); r++) {
            uint64_t types = readMsr(fixedRanges[r].msr);
            for (uint64_t i = 0; i < 8; i++) {
                uint64_t type = (types >> (8 * i)) & 0xFF;
                uint64_t start = fixedRanges[r].start + i * fixedRanges[r].step;
                uint64_t end = start + fixedRanges[r].step;
                for (uint64_t address = start; address < end; address += PAGE_SIZE) {
                    pt[address / PAGE_SIZE].fields.memoryType = type;
                }
            }
        }
    }

    logInfo("result: RAM 0x0 %s, VGA 0xa0000 %s, kernel 0x100000 %s, APIC 0xfee00000 %s\n",
            memoryTypeName(pt[0x0 / PAGE_SIZE].fields.memoryType),
            memoryTypeName(pt[0xA0000 / PAGE_SIZE].fields.memoryType),
            memoryTypeName(pt[0x100000 / PAGE_SIZE].fields.memoryType),
            memoryTypeName(pt[0xFEE00000 / PAGE_SIZE].fields.memoryType));
}

static void linkTable(EptTableEntry *entry, void *nextTable) {
    entry->fields.read = 1;
    entry->fields.write = 1;
    entry->fields.execute = 1;
    entry->fields.nextTableFrame = (uint64_t)nextTable >> 12;
}

/* Walks the tables the same way the CPU will once EPT is enabled, to
 * check the linking before anything depends on it. */
static uint64_t translate(EptTableEntry *pml4, uint64_t guestPhysical) {
    EptTableEntry *pdpt = (EptTableEntry *)((uint64_t)pml4[(guestPhysical >> 39) & 0x1FF].fields.nextTableFrame << 12);
    EptTableEntry *pd = (EptTableEntry *)((uint64_t)pdpt[(guestPhysical >> 30) & 0x1FF].fields.nextTableFrame << 12);
    EptPageEntry *pt = (EptPageEntry *)((uint64_t)pd[(guestPhysical >> 21) & 0x1FF].fields.nextTableFrame << 12);
    uint64_t frame = pt[(guestPhysical >> 12) & 0x1FF].fields.pageFrame;
    return (frame << 12) | (guestPhysical & 0xFFF);
}

EptTableEntry *initializeEpt(void) {
    uint64_t pdCount = EPT_MAPPED_GB;                 /* one PD per GB */
    uint64_t ptCount = pdCount * ENTRIES_PER_TABLE;   /* one PT per 2MB */
    uint64_t pageCount = ptCount * ENTRIES_PER_TABLE; /* one PTE per 4KB */

    /* Only PML4[0] is used (it covers 512GB), and only EPT_MAPPED_GB
     * entries of the PDPT. PDs and PTs are allocated as one contiguous
     * block each so entry i of one level maps to table i of the next. */
    EptTableEntry *pml4 = allocateMemory(PAGE_SIZE);
    EptTableEntry *pdpt = allocateMemory(PAGE_SIZE);
    EptTableEntry *pd = allocateMemory(pdCount * PAGE_SIZE);
    EptPageEntry *pt = allocateMemory(ptCount * PAGE_SIZE);
    if (!pml4 || !pdpt || !pd || !pt) {
        logError("EPT: not enough memory for the tables!!!\n");
        return 0;
    }

    linkTable(&pml4[0], pdpt);
    for (uint64_t i = 0; i < pdCount; i++) {
        linkTable(&pdpt[i], &pd[i * ENTRIES_PER_TABLE]);
    }
    for (uint64_t i = 0; i < ptCount; i++) {
        linkTable(&pd[i], &pt[i * ENTRIES_PER_TABLE]);
    }
    for (uint64_t i = 0; i < pageCount; i++) {
        pt[i].fields.read = 1;
        pt[i].fields.write = 1;
        pt[i].fields.execute = 1;
        pt[i].fields.pageFrame = i;
    }

    /* Low memory, the kernel's own code, the top of the E820 RAM
     * region, and the Local APIC MMIO page (with a non-zero offset, to
     * check the offset survives). */
    uint64_t checks[] = { 0x0, 0x101906, 0x7FFDF123, 0xFEE00ABC };
    for (uint64_t i = 0; i < sizeof(checks) / sizeof(checks[0]); i++) {
        uint64_t result = translate(pml4, checks[i]);
        if (result != checks[i]) {
            logError("EPT: %lx translated to %lx, expected identity!!!\n", checks[i], result);
            return 0;
        }
    }

    logInfo("EPT tables initialized: %lu GB identity-mapped, 4KB pages, PML4 at %p\n",
            (unsigned long)EPT_MAPPED_GB, pml4);

    /* After the log above, so boot output follows the order the work
     * happens in: tables first, then their caching policy. */
    applyMtrrMemoryTypes(pt, pageCount);
    return pml4;
}

void hideEptRange(EptTableEntry *pml4, uint64_t base, uint64_t end) {
    /* Covers the full 4KB page each endpoint touches - a reserved range
     * that doesn't start or end on a page boundary still gets the whole
     * page behind it hidden, which is what callers want. */
    uint64_t pageStart = base & ~(PAGE_SIZE - 1);
    uint64_t pageEnd = (end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint64_t hidden = 0;

    for (uint64_t addr = pageStart; addr < pageEnd; addr += PAGE_SIZE) {
        /* Walk PML4 -> PDPT -> PD -> PT, same shape as translate(). The
         * EPT is 4KB-paged throughout, so every page has a PTE and no
         * entry needs to be split. */
        EptTableEntry *pdpt = (EptTableEntry *)((uint64_t)pml4[(addr >> 39) & 0x1FF].fields.nextTableFrame << 12);
        EptTableEntry *pd   = (EptTableEntry *)((uint64_t)pdpt[(addr >> 30) & 0x1FF].fields.nextTableFrame << 12);
        EptPageEntry *pt    = (EptPageEntry *) ((uint64_t)pd[(addr >> 21) & 0x1FF].fields.nextTableFrame << 12);
        EptPageEntry *pte   = &pt[(addr >> 12) & 0x1FF];
        pte->fields.read = 0;
        pte->fields.write = 0;
        pte->fields.execute = 0;
        hidden++;
    }

    logInfo("EPT: hid %lu pages covering %lx-%lx\n", hidden, pageStart, pageEnd);
}

#define IA32_VMX_EPT_VPID_CAP_MSR 0x48Cu
#define EPT_CAP_WALK_LENGTH_4     (1ull << 6)
#define EPT_CAP_MEMORY_TYPE_UC    (1ull << 8)
#define EPT_CAP_MEMORY_TYPE_WB    (1ull << 14)

uint64_t buildEptPointer(EptTableEntry *pml4) {
    uint64_t capability = readMsr(IA32_VMX_EPT_VPID_CAP_MSR);
    if (!(capability & EPT_CAP_WALK_LENGTH_4)) {
        logError("EPT: CPU doesn't support a 4-level EPT walk!!!\n");
        return 0;
    }

    EptPointer eptPointer = { .value = 0 };
    /* The EPT tables are ordinary RAM, so WB is the right type for the
     * CPU's own reads of them - UC only as a fallback. */
    if (capability & EPT_CAP_MEMORY_TYPE_WB) {
        eptPointer.fields.memoryType = MEMORY_TYPE_WB;
    } else if (capability & EPT_CAP_MEMORY_TYPE_UC) {
        eptPointer.fields.memoryType = MEMORY_TYPE_UC;
    } else {
        logError("EPT: CPU supports neither WB nor UC for the EPT tables!!!\n");
        return 0;
    }
    eptPointer.fields.pageWalkLengthMinusOne = 4 - 1;
    eptPointer.fields.pml4Frame = (uint64_t)pml4 >> 12;

    logInfo("EPT pointer: %lx (4-level walk, %s)\n",
            eptPointer.value, memoryTypeName(eptPointer.fields.memoryType));
    return eptPointer.value;
}
