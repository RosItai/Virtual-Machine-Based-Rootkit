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

/* Off by default - set to 1 to log every allocation when debugging. */
#ifndef DEBUG_ALLOCATIONS
#define DEBUG_ALLOCATIONS 0
#endif

/* Laid down by the linker script, so they're addresses, not variables -
 * taking the address of each is the only correct way to read one. */
extern uint8_t __kernel_start[];
extern uint8_t __kernel_end[];

/* The running union of every allocation, kept here because this is the
 * only code that knows where they land. Seeded from the kernel image
 * itself: that memory is just as much the VMM's, and just as reachable
 * by a guest, even though no allocator handed it out. */
static uint64_t reservedBase = 0;
static uint64_t reservedEnd = 0;

static void recordReservedRange(uint64_t base, uint64_t end) {
    if (reservedEnd == 0) {
        reservedBase = (uint64_t)__kernel_start;
        reservedEnd = (uint64_t)__kernel_end;
    }
    if (base < reservedBase) {
        reservedBase = base;
    }
    if (end > reservedEnd) {
        reservedEnd = end;
    }
}

int getVmmReservedRange(uint64_t *base, uint64_t *end) {
    if (reservedEnd == 0) {
        /* Nothing allocated yet, but the kernel image is already there
         * and already needs protecting. */
        *base = (uint64_t)__kernel_start;
        *end = (uint64_t)__kernel_end;
        return 1;
    }
    *base = reservedBase;
    *end = reservedEnd;
    return 1;
}

void *allocateMemory(uint64_t length) {
    MemoryMapTable *table = (MemoryMapTable *)MEMORY_MAP_TABLE_ADDR;
    uint64_t len = ALIGN_UP(length, PAGE_SIZE);

    /* Last usable region with enough room wins - simple, not best-fit,
     * good enough while every allocation still happens at boot time. */
    int32_t chosen = -1;
    for (uint32_t i = 0; i < table->numberOfEntries; i++) {
        if (table->entries[i].type == E820_USABLE &&
            table->entries[i].length >= len) {
            chosen = (int32_t)i;
        }
    }
    if (chosen < 0) {
        return 0;
    }

    MemoryMapEntry *region = &table->entries[chosen];
    uint64_t regionEnd = region->baseAddress + region->length;
    uint64_t allocationEnd = ALIGN_DOWN(regionEnd, PAGE_SIZE);
    void *allocationAddress = (void *)(allocationEnd - len);

    region->length -= len;
    memset(allocationAddress, 0, len);
    recordReservedRange((uint64_t)allocationAddress, allocationEnd);

#if DEBUG_ALLOCATIONS
    logInfo("allocateMemory: %lu bytes at %p (region %d, %lu bytes left there)\n",
            len, allocationAddress, chosen, region->length);
#endif

    return allocationAddress;
}
