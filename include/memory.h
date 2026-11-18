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

/* The span of physical memory the VMM has taken for itself: the
 * kernel image (__kernel_start/__kernel_end from the linker script)
 * unioned with everything allocateMemory has handed out so far.
 *
 * This is what has to be hidden from the guest - declared reserved in
 * the E820 map it sees, and eventually unmapped from the EPT. Both
 * callers want one contiguous span rather than a list, so overlapping
 * nothing is traded for simplicity: if allocations ever straddled two
 * separate E820 regions, the span would also cover the gap between
 * them. That errs towards protecting too much, which is the safe
 * direction - the guest loses a little RAM, nothing breaks.
 *
 * `end` is exclusive, and both are page-aligned. Returns 0 if
 * nothing has been allocated yet (`base`/`end` are then untouched). */
int getVmmReservedRange(uint64_t *base, uint64_t *end);

#endif
