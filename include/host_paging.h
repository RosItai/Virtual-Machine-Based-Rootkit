/* host_paging.h - see host_paging.c */
#ifndef HOST_PAGING_H
#define HOST_PAGING_H

#include <stdint.h>

/* Rebuilds the host's identity-mapped page tables in memory from
 * allocateMemory and switches CR3 to them. Returns the new PML4, or 0
 * if allocation failed (the old tables stay in use then). */
uint64_t *initializeHostPaging(void);

#endif
