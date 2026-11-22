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
#include "disk.h"

/* The int 15h, E820 protocol. EDX carries this signature both ways; a
 * call without it isn't really an E820 query. */
#define E820_SIGNATURE 0x534D4150u /* 'SMAP' */
#define INT15_FUNCTION_E820 0xE820u
#define E820_ENTRY_SIZE 20u /* our map is the 20-byte form - see memory_map.h */

/* Where the stubs live in guest memory. The original 0x8000/0x8100
 * placement was NOT safe: a Windows NTFS VBR is 16 sectors loaded
 * sequentially at 0x7C00, so its sector 2 (LBA 2050) lands exactly on
 * 0x8000-0x81FF and overwrites both stubs - whereupon the IVT entries
 * still point there and INT 13h/15h vectors dive into random VBR bytes
 * and silently hang (verified: VBR stops right after the LBA 2050 read
 * that clobbered the stubs, no further disk I/O, no VM exits).
 *
 * 0x9F000/0x9F100 sits above the VBR's reach (0x7C00 + 16*512 = 0x9C00)
 * and below SeaBIOS's EBDA at 0x9FC00. The HOOK_STUB_PROTECT_* pair
 * below is added to the guest's E820 as reserved so bootmgr and the
 * kernel both leave this page alone later too. */
#define HOOK_STUB_SEGMENT 0x9F00u
#define HOOK_STUB_ADDRESS ((uint64_t)HOOK_STUB_SEGMENT << 4)

/* IVT entry for INT 15h: 4 bytes (offset then segment) at vector * 4. */
#define INT15_IVT_ENTRY 0x54u /* 0x15 * 4 */

/* INT 13h stub sits one paragraph (256 bytes) above the INT 15h one. */
#define HOOK13_STUB_SEGMENT 0x9F10u
#define HOOK13_STUB_ADDRESS ((uint64_t)HOOK13_STUB_SEGMENT << 4)
#define INT13_IVT_ENTRY 0x4Cu /* 0x13 * 4 */

/* The page the guest must see as reserved so it won't overwrite the
 * stubs: covers both of them plus a bit of slack, and stops at the
 * EBDA boundary SeaBIOS already owns. */
#define HOOK_STUB_PROTECT_BASE 0x9F000u
#define HOOK_STUB_PROTECT_END  0x9FC00u

/* INT 13h function numbers we recognise for logging (AH on entry). */
#define INT13_FN_CHS_READ 0x02u
#define INT13_FN_EXT_READ 0x42u

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
    /* One extra slot past VMM_MAX_RESERVED_RANGES for the hook-stub page
     * this file itself needs to protect (see HOOK_STUB_PROTECT_*). */
    MemoryRange reserved[VMM_MAX_RESERVED_RANGES + 1];
    int reservedCount = getVmmReservedRanges(reserved, VMM_MAX_RESERVED_RANGES);
    reserved[reservedCount].base = HOOK_STUB_PROTECT_BASE;
    reserved[reservedCount].end  = HOOK_STUB_PROTECT_END;
    reservedCount++;

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

/* The INT 13h stub blob and its patchable far-pointer, from
 * int13_hook.asm. */
extern uint8_t int13_hook_start[];
extern uint8_t int13_hook_end[];
extern uint8_t int13_hook_original_vector[];

static void installInt13Hook(void) {
    uint64_t stubSize = (uint64_t)(int13_hook_end - int13_hook_start);
    memcpy((void *)HOOK13_STUB_ADDRESS, int13_hook_start, stubSize);

    uint16_t *ivt = (uint16_t *)(uint64_t)INT13_IVT_ENTRY;
    uint16_t originalOffset = ivt[0];
    uint16_t originalSegment = ivt[1];

    uint64_t vectorOffset = (uint64_t)(int13_hook_original_vector - int13_hook_start);
    uint16_t *stubVector = (uint16_t *)(HOOK13_STUB_ADDRESS + vectorOffset);
    stubVector[0] = originalOffset;
    stubVector[1] = originalSegment;

    ivt[0] = 0;                   /* stub offset within its segment */
    ivt[1] = HOOK13_STUB_SEGMENT;

    logInfo("INT 13h hooked: stub at %lx (%lu bytes), original handler %x:%x chained\n",
            HOOK13_STUB_ADDRESS, stubSize, originalSegment, originalOffset);
}

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
    ivt[1] = HOOK_STUB_SEGMENT;

    logInfo("INT 15h hooked: stub at %lx (%lu bytes), original handler %x:%x chained\n",
            HOOK_STUB_ADDRESS, stubSize, originalSegment, originalOffset);

    /* Observe the firmware boot chain's disk access too. */
    installInt13Hook();
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

static void serviceDiskObserve(VmExitContext *ctx) {
    GuestRegisters *regs = ctx->registers;
    uint8_t function = (uint8_t)(regs->rax >> 8); /* AH */
    uint8_t drive = (uint8_t)regs->rdx;           /* DL */

    if (function == INT13_FN_EXT_READ) {
        /* DS:SI points at the Disk Address Packet (guest-physical, which
         * the identity EPT/host paging map 1:1, so it's a usable ptr). */
        uint64_t dapPhys = vmread(GUEST_DS_BASE) + (regs->rsi & 0xFFFFu);
        const dap_t *dap = (const dap_t *)dapPhys;
        logInfo("guest INT 13h: ext read drive 0x%x, LBA %lu, %u sector(s)\n",
                drive, (unsigned long)dap->sector, dap->amount);
    } else if (function == INT13_FN_CHS_READ) {
        logInfo("guest INT 13h: CHS read drive 0x%x, %u sector(s)\n",
                drive, (unsigned int)(regs->rax & 0xFFu) /* AL = count */);
    } else {
        logInfo("guest INT 13h: function 0x%x, drive 0x%x\n", function, drive);
    }
}

int handleHypercall(VmExitContext *ctx) {
    uint32_t magic = (uint32_t)ctx->registers->rbp;

    if (magic == VMBR_HYPERCALL_MAGIC) {
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

    if (magic == VMBR_DISK_HYPERCALL_MAGIC) {
        serviceDiskObserve(ctx);
        return 1;
    }

    return 0; /* not ours - the caller turns this into a #UD */
}
