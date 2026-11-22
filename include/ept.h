/* ept.h - EPT (Extended Page Table) structures, see ept.c.
 * Bit layouts per Intel SDM Vol 3C, "EPT Translation Mechanism".
 */
#ifndef EPT_H
#define EPT_H

#include <stdint.h>

/* A PML4E, PDPTE or PDE that points at the next table level (not a
 * large page - bit 7 stays 0 inside "reserved"). */
typedef union {
    uint64_t value;
    struct {
        uint64_t read : 1;
        uint64_t write : 1;
        uint64_t execute : 1;
        uint64_t reserved : 5;        /* bits 3-7 must be 0 */
        uint64_t accessed : 1;
        uint64_t ignored0 : 1;
        uint64_t userExecute : 1;
        uint64_t ignored1 : 1;
        uint64_t nextTableFrame : 40; /* next table's physical address >> 12 */
        uint64_t ignored2 : 12;
    } fields;
} EptTableEntry;

/* A PTE mapping one 4KB page. */
typedef union {
    uint64_t value;
    struct {
        uint64_t read : 1;
        uint64_t write : 1;
        uint64_t execute : 1;
        uint64_t memoryType : 3;      /* from the MTRRs, see applyMtrrMemoryTypes in ept.c */
        uint64_t ignorePat : 1;
        uint64_t ignored0 : 1;
        uint64_t accessed : 1;
        uint64_t dirty : 1;
        uint64_t userExecute : 1;
        uint64_t ignored1 : 1;
        uint64_t pageFrame : 40;      /* mapped page's physical address >> 12 */
        uint64_t ignored2 : 11;
        uint64_t suppressVe : 1;
    } fields;
} EptPageEntry;

/* The EPTP - the VMCS field that points the CPU at the EPT. */
typedef union {
    uint64_t value;
    struct {
        uint64_t memoryType : 3;             /* type used to access the EPT tables themselves */
        uint64_t pageWalkLengthMinusOne : 3; /* 3 = 4-level walk (PML4 -> PDPT -> PD -> PT) */
        uint64_t enableAccessedDirty : 1;
        uint64_t supervisorShadowStack : 1;
        uint64_t reserved0 : 4;
        uint64_t pml4Frame : 40;             /* PML4's physical address >> 12 */
        uint64_t reserved1 : 12;
    } fields;
} EptPointer;

/* Allocates and links an identity-mapped EPT covering the first 4GB
 * with 4KB pages. Returns the PML4, or 0 if allocation failed. */
EptTableEntry *initializeEpt(void);

/* Builds the EPTP value for EPT_POINTER, after checking the CPU
 * supports a 4-level walk and a usable memory type. 0 on failure. */
uint64_t buildEptPointer(EptTableEntry *pml4);

/* Walks `pml4` and clears R/W/X on every 4KB page in [base, end). Once
 * the guest is running, any access to those pages exits with
 * EXIT_REASON_EPT_VIOLATION - which is how we keep a guest OS from
 * silently corrupting VMBR's own pages when it ignores the "reserved"
 * markings in the E820 map it sees. */
void hideEptRange(EptTableEntry *pml4, uint64_t base, uint64_t end);

#endif
