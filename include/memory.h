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

#endif
