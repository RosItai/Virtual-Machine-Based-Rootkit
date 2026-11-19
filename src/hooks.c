/* hooks.c - host side of the guest hooks (see hooks.h).
 *
 * The INT 15h / E820 hook has two halves. This file is the host half:
 *   - buildGuestE820Map: the real BIOS map with the VMM's own memory
 *     carved out as reserved, computed once before launch.
 *   - installGuestHooks: splice the real-mode stub into the guest's IVT.
 *   - handleHypercall / serviceE820: answer the VMCALL the stub makes.
 * The guest half is the stub in int15_hook.asm.
 */
#include <stdint.h>
#include "hooks.h"
#include "vmexit.h"
#include "vmcs.h"
#include "memory.h"
#include "memory_map.h"
#include "string.h"
#include "debug.h"

/* The int 15h, E820 protocol. EDX carries this signature both ways; a
 * call without it isn't really an E820 query. */
#define E820_SIGNATURE 0x534D4150u /* 'SMAP' */
#define INT15_FUNCTION_E820 0xE820u
#define E820_ENTRY_SIZE 20u /* our map is the 20-byte form - see memory_map.h */

/* Where the stub lives in guest memory: physical 0x8000, i.e. real-mode
 * 0x0800:0000. Free conventional RAM - above the guest at 0x7000 and
 * its stack (which grows down from 0x7000, away from here), below the
 * 0x7C00 boot area. */
#define HOOK_STUB_SEGMENT 0x0800u
#define HOOK_STUB_ADDRESS ((uint64_t)HOOK_STUB_SEGMENT << 4)

/* IVT entry for INT 15h: 4 bytes (offset then segment) at vector * 4. */
#define INT15_IVT_ENTRY 0x54u /* 0x15 * 4 */

/* ===================================================================
 * The filtered E820 map the guest sees
 * =================================================================== */

/* Room for the real map (7 entries in QEMU) plus the splits a couple of
 * carved-out reserved ranges add - comfortably over-sized. */
#define MAX_GUEST_E820_ENTRIES 32u

static MemoryMapEntry guestE820[MAX_GUEST_E820_ENTRIES];
static uint32_t guestE820Count;

static void emitEntry(uint64_t base, uint64_t length, uint32_t type) {
    if (length == 0) {
        return;
    }
    if (guestE820Count >= MAX_GUEST_E820_ENTRIES) {
        logError("guest E820 map overflow - more than %u entries\n", MAX_GUEST_E820_ENTRIES);
        return;
    }
    guestE820[guestE820Count].baseAddress = base;
    guestE820[guestE820Count].length = length;
    guestE820[guestE820Count].type = type;
    guestE820Count++;
}

/* Splits one usable region against the reserved ranges, emitting the
 * free slices as usable and the overlaps as reserved. Walks a cursor
 * from the region's base to its end, at each step finding the
 * earliest-starting reserved range that still overlaps what's left. */
static void carveUsableRegion(uint64_t base, uint64_t length,
                              const MemoryRange *reserved, int reservedCount) {
    uint64_t end = base + length;
    uint64_t cursor = base;

    while (cursor < end) {
        uint64_t holeBase = end;
        uint64_t holeEnd = end;
        int found = 0;

        for (int i = 0; i < reservedCount; i++) {
            uint64_t rb = reserved[i].base;
            uint64_t re = reserved[i].end;
            if (re <= cursor || rb >= end) {
                continue; /* doesn't overlap the part still ahead of us */
            }
            if (rb < cursor) rb = cursor; /* clamp into the region */
            if (re > end) re = end;
            if (!found || rb < holeBase) {
                holeBase = rb;
                holeEnd = re;
                found = 1;
            }
        }

        if (!found) {
            emitEntry(cursor, end - cursor, E820_USABLE);
            break;
        }
        emitEntry(cursor, holeBase - cursor, E820_USABLE); /* free slice before the hole */
        emitEntry(holeBase, holeEnd - holeBase, E820_RESERVED);
        cursor = holeEnd;
    }
}

static void buildGuestE820Map(void) {
    MemoryMapTable *source = (MemoryMapTable *)MEMORY_MAP_TABLE_ADDR;
    MemoryRange reserved[VMM_MAX_RESERVED_RANGES];
    int reservedCount = getVmmReservedRanges(reserved, VMM_MAX_RESERVED_RANGES);

    guestE820Count = 0;
    for (uint32_t i = 0; i < source->numberOfEntries; i++) {
        MemoryMapEntry *entry = &source->entries[i];
        if (entry->type == E820_USABLE) {
            carveUsableRegion(entry->baseAddress, entry->length, reserved, reservedCount);
        } else {
            /* Non-usable entries pass through untouched - the guest was
             * never going to allocate from them anyway. */
            emitEntry(entry->baseAddress, entry->length, entry->type);
        }
    }

    logInfo("guest E820 map built: %u entries (%d VMM ranges carved out as reserved)\n",
            guestE820Count, reservedCount);
    for (int i = 0; i < reservedCount; i++) {
        logInfo("  protecting %lx-%lx\n", reserved[i].base, reserved[i].end);
    }
}

/* ===================================================================
 * Installing the hook
 * =================================================================== */

/* The stub blob and the offset of its patchable far-pointer, from
 * int15_hook.asm. */
extern uint8_t int15_hook_start[];
extern uint8_t int15_hook_end[];
extern uint8_t int15_hook_original_vector[];

void installGuestHooks(void) {
    buildGuestE820Map();

    /* Copy the stub into guest memory (identity-mapped, so the physical
     * address is a usable pointer here). */
    uint64_t stubSize = (uint64_t)(int15_hook_end - int15_hook_start);
    memcpy((void *)HOOK_STUB_ADDRESS, int15_hook_start, stubSize);

    /* The IVT entry is offset(16) then segment(16). Save the original
     * so the stub can chain non-E820 calls to the real BIOS handler,
     * then repoint the vector at our stub. */
    uint16_t *ivt = (uint16_t *)(uint64_t)INT15_IVT_ENTRY;
    uint16_t originalOffset = ivt[0];
    uint16_t originalSegment = ivt[1];

    /* Patch the saved far pointer into the stub's copy. The field's
     * position inside the blob is the same at its runtime address. */
    uint64_t vectorOffset = (uint64_t)(int15_hook_original_vector - int15_hook_start);
    uint16_t *stubVector = (uint16_t *)(HOOK_STUB_ADDRESS + vectorOffset);
    stubVector[0] = originalOffset;
    stubVector[1] = originalSegment;

    ivt[0] = 0;                 /* stub offset within its segment */
    ivt[1] = HOOK_STUB_SEGMENT; /* 0x0800 */

    logInfo("INT 15h hooked: stub at %lx (%lu bytes), original handler %x:%x chained\n",
            HOOK_STUB_ADDRESS, stubSize, originalSegment, originalOffset);
}

/* ===================================================================
 * Answering the hypercall
 * =================================================================== */

static void setGuestCarry(int set) {
    uint64_t rflags = vmread(GUEST_RFLAGS);
    if (set) {
        rflags |= CARRY_FLAG_MASK;
    } else {
        rflags &= ~CARRY_FLAG_MASK;
    }
    vmwrite(GUEST_RFLAGS, rflags);
}

/* The guest called INT 15h / E820; the stub bounced it to us. Registers
 * hold the standard E820 arguments. We copy one filtered entry into the
 * guest's ES:DI buffer and set up the next continuation. */
static void serviceE820(VmExitContext *ctx) {
    GuestRegisters *regs = ctx->registers;
    uint32_t index = (uint32_t)regs->rbx;      /* continuation, 0 to start */
    uint32_t bufferLength = (uint32_t)regs->rcx;
    uint32_t signature = (uint32_t)regs->rdx;

    if (signature != E820_SIGNATURE || bufferLength < E820_ENTRY_SIZE ||
        index >= guestE820Count) {
        /* A bad signature, a too-small buffer, or an index past the end
         * is how the real BIOS reports "no more / invalid" too. */
        setGuestCarry(1);
        return;
    }

    /* ES:DI in real mode - base is ES<<4, held in the VMCS. The result
     * is a guest-physical address, which the identity EPT and the
     * identity host paging both map 1:1, so it's a usable pointer. */
    uint64_t bufferPhys = vmread(GUEST_ES_BASE) + (regs->rdi & 0xFFFFu);
    memcpy((void *)bufferPhys, &guestE820[index], E820_ENTRY_SIZE);

    regs->rax = E820_SIGNATURE;  /* the BIOS echoes 'SMAP' back in EAX */
    regs->rcx = E820_ENTRY_SIZE; /* bytes actually written */
    regs->rbx = (index + 1 < guestE820Count) ? index + 1 : 0; /* 0 = last entry */
    setGuestCarry(0);
}

int handleHypercall(VmExitContext *ctx) {
    if ((uint32_t)ctx->registers->rbp != VMBR_HYPERCALL_MAGIC) {
        return 0; /* not ours - the caller turns this into a #UD */
    }

    switch ((uint32_t)ctx->registers->rax) {
    case INT15_FUNCTION_E820:
        serviceE820(ctx);
        break;
    default:
        logError("unknown VMBR hypercall, function %lx\n", ctx->registers->rax & 0xFFFFFFFFu);
        setGuestCarry(1);
        break;
    }
    return 1;
}
