/* memory.h - see memory.c */
#ifndef MEMORY_H
#define MEMORY_H

#include <stdint.h>

#define PAGE_SIZE 4096ull

/* Returns a zeroed, page-aligned block of at least `length` bytes
 * (rounded up to a 4KB multiple), carved out of the BIOS E820 memory
 * map. Returns 0 if no usable region has enough room left. There is
 * no matching free() - this is a simple bump allocator for early
 * kernel/hypervisor bring-up. */
void *allocateMemory(uint64_t length);

/* A half-open physical range, [base, end), both page-aligned. */
typedef struct {
    uint64_t base;
    uint64_t end;
} MemoryRange;

/* The kernel image and the allocator footprint are far enough apart to
 * be worth reporting as two ranges, never one enclosing span. */
#define VMM_MAX_RESERVED_RANGES 2

/* Fills `ranges` with the physical ranges the VMM has taken for
 * itself - the kernel image (always), and the allocator's footprint
 * (once anything has been allocated) - and returns how many it wrote,
 * never more than maxRanges. This is what has to be hidden from the
 * guest: marked reserved in the E820 map it sees (see hooks.c) and,
 * later, unmapped from the EPT. */
int getVmmReservedRanges(MemoryRange *ranges, int maxRanges);

#endif
