/* host_paging.c
 * Rebuilds the host's page tables outside the kernel image. The boot
 * tables (paging.asm) sit in the kernel's BSS right above 1MB - low
 * memory a guest OS like Windows will use and overwrite. These come
 * from allocateMemory instead, i.e. the top of the largest usable
 * E820 region.
 *
 * Same mapping as paging.asm: the first 4GB, identity-mapped with 2MB
 * pages, so switching CR3 over is seamless - every address means the
 * same thing before and after.
 *
 * Named host_paging.c rather than paging.c: the Makefile builds
 * src/X.c and src/X.asm both into build/X.o, so paging.c would
 * collide with paging.asm.
 */
#include <stdint.h>
#include "host_paging.h"
#include "memory.h"
#include "cpu.h"
#include "debug.h"

#define HOST_MAPPED_GB    8ull /* keep in sync with MEM_SIZE_GB in paging.asm */
#define ENTRIES_PER_TABLE 512ull
#define LARGE_PAGE_SIZE   0x200000ull

#define PTE_PRESENT   (1ull << 0)
#define PTE_WRITABLE  (1ull << 1)
#define PTE_PAGE_SIZE (1ull << 7) /* in a PDE: maps a 2MB page directly */

uint64_t *initializeHostPaging(void) {
    uint64_t *pml4 = allocateMemory(PAGE_SIZE);
    uint64_t *pdpt = allocateMemory(PAGE_SIZE);
    uint64_t *pd = allocateMemory(HOST_MAPPED_GB * PAGE_SIZE); /* one PD per GB */
    if (!pml4 || !pdpt || !pd) {
        logError("Not enough memory for the host page tables!!!\n");
        return 0;
    }

    /* Regular page-table entries hold the address in place (bits
     * 51:12), so a page-aligned pointer ORed with flags is the entry -
     * unlike the EPT structs in ept.h, which store address >> 12. */
    pml4[0] = (uint64_t)pdpt | PTE_PRESENT | PTE_WRITABLE;
    for (uint64_t i = 0; i < HOST_MAPPED_GB; i++) {
        pdpt[i] = (uint64_t)&pd[i * ENTRIES_PER_TABLE] | PTE_PRESENT | PTE_WRITABLE;
    }
    for (uint64_t i = 0; i < HOST_MAPPED_GB * ENTRIES_PER_TABLE; i++) {
        pd[i] = (i * LARGE_PAGE_SIZE) | PTE_PRESENT | PTE_WRITABLE | PTE_PAGE_SIZE;
    }

    /* Safe to switch while running: the mapping is identical, and the
     * CR3 write flushes the TLB (no global pages are in use). */
    uint64_t oldCr3 = readCr3();
    writeCr3((uint64_t)pml4);

    logInfo("Host page tables moved from %lx to %p (%lu GB identity-mapped, 2MB pages)\n",
            oldCr3, pml4, (unsigned long)HOST_MAPPED_GB);
    return pml4;
}
