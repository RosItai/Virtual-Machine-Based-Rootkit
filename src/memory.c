/* memory.c
 * A simple bump-style physical memory allocator, ported from a
 * reference implementation. allocateMemory hands out page-aligned,
 * zeroed blocks carved from the top of a usable BIOS E820 region,
 * shrinking that region's recorded length so later calls don't reuse
 * the same space. No freeing, no fine-grained tracking - meant for
 * early bring-up, before anything fancier (a real page frame
 * allocator, EPT-aware bookkeeping) is needed.
 *
 * Differences from the reference this was ported from:
 *   - Guards against no usable region being found at all (the
 *     original left its "chosen" index at an out-of-bounds value in
 *     that case and indexed the entries array with it anyway).
 *   - Allocates from a page-aligned copy of the region's end address
 *     (rounded down) rather than page-aligning the computed
 *     allocation address afterward - rounding the address up post hoc
 *     could push it (and the block above it) past the region's actual
 *     end if that end wasn't already page-aligned.
 *   - >= instead of > when checking a region is big enough, so an
 *     exact-fit region isn't skipped.
 *   - The per-entry debug print now only covers the region actually
 *     chosen, gated behind DEBUG_ALLOCATIONS, rather than printing
 *     every entry on every call regardless of that flag.
 */
#include <stdint.h>
#include "memory.h"
#include "memory_map.h"
#include "string.h"
#include "debug.h"

#define ALIGN_UP(x, align)   (((x) + (align) - 1) & ~((align) - 1))
#define ALIGN_DOWN(x, align) ((x) & ~((align) - 1))

/* Upper bound on any address the allocator may return. Both the boot
 * page tables (paging.asm, MEM_SIZE_GB=4) and the host page tables
 * initializeHostPaging builds (HOST_MAPPED_GB=4) identity-map only the
 * first 4GB - handing out an address above that triple-faults the moment
 * the allocator memsets it. With `-m 4G` QEMU splits RAM around the PCI
 * hole: ~3GB below 4GB, the rest above - and that above-4GB chunk is
 * the LAST usable E820 region, which the picker below would otherwise
 * prefer. Clamp regionEnd at the 4GB mark instead. */
#define ALLOC_LIMIT 0x100000000ull

/* Off by default - set to 1 to log every allocation when debugging. */
#ifndef DEBUG_ALLOCATIONS
#define DEBUG_ALLOCATIONS 0
#endif

/* Laid down by the linker script, so they're addresses, not variables -
 * taking the address of each is the only correct way to read one. */
extern uint8_t __kernel_start[];
extern uint8_t __kernel_end[];

/* The allocator's footprint, tracked separately from the kernel image
 * rather than merged into one span. The two sit far apart - the kernel
 * at 1MB, the allocations near the top of RAM - so a single enclosing
 * range would cover the ~2GB of guest RAM between them and leave the
 * guest almost nothing. Every allocation is a bump downward from the
 * top of one region, so they stay contiguous and [allocLow, allocHigh)
 * is tight. allocHigh == 0 means nothing has been allocated yet. */
static uint64_t allocLow = 0;
static uint64_t allocHigh = 0;

static void recordAllocation(uint64_t base, uint64_t end) {
    if (allocHigh == 0 || end > allocHigh) {
        allocHigh = end;
    }
    if (allocLow == 0 || base < allocLow) {
        allocLow = base;
    }
}

int getVmmReservedRanges(MemoryRange *ranges, int maxRanges) {
    int count = 0;

    /* The kernel image always needs protecting - it's the VMM's code
     * and data, sitting in ordinary RAM a guest can reach. */
    if (count < maxRanges) {
        ranges[count].base = (uint64_t)__kernel_start;
        ranges[count].end = (uint64_t)__kernel_end;
        count++;
    }

    /* The allocations (EPT tables, host stack, VMXON/VMCS, page
     * tables) are a second, separate range. */
    if (allocHigh != 0 && count < maxRanges) {
        ranges[count].base = allocLow;
        ranges[count].end = allocHigh;
        count++;
    }

    return count;
}

void *allocateMemory(uint64_t length) {
    MemoryMapTable *table = (MemoryMapTable *)MEMORY_MAP_TABLE_ADDR;
    uint64_t len = ALIGN_UP(length, PAGE_SIZE);

    /* Last usable region with enough room wins - simple, not best-fit,
     * good enough while every allocation still happens at boot time.
     * Clamped to ALLOC_LIMIT: a region's effective top is min(real top,
     * 4GB), and it has to fit `len` below that ceiling - otherwise an
     * above-4GB region wins and memset faults on the first page. */
    int32_t chosen = -1;
    uint64_t chosenEnd = 0;
    for (uint32_t i = 0; i < table->numberOfEntries; i++) {
        if (table->entries[i].type != E820_USABLE) continue;
        uint64_t base = table->entries[i].baseAddress;
        if (base >= ALLOC_LIMIT) continue;
        uint64_t end = base + table->entries[i].length;
        if (end > ALLOC_LIMIT) end = ALLOC_LIMIT;
        if (end - base < len) continue;
        chosen = (int32_t)i;
        chosenEnd = end;
    }
    if (chosen < 0) {
        return 0;
    }

    MemoryMapEntry *region = &table->entries[chosen];
    uint64_t allocationEnd = ALIGN_DOWN(chosenEnd, PAGE_SIZE);
    void *allocationAddress = (void *)(allocationEnd - len);

    /* Shrink the region from the top by however much we just took,
     * including the amount we clipped off above the 4GB line - the
     * region above the clip is unreachable anyway, so hiding it keeps
     * later calls honest about what's still available. */
    region->length -= (region->baseAddress + region->length) - (allocationEnd - len);
    memset(allocationAddress, 0, len);
    recordAllocation((uint64_t)allocationAddress, allocationEnd);

#if DEBUG_ALLOCATIONS
    logInfo("allocateMemory: %lu bytes at %p (region %d, %lu bytes left there)\n",
            len, allocationAddress, chosen, region->length);
#endif

    return allocationAddress;
}
