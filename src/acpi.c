/* acpi.c
 * Finds the ACPI RSDP, walks it to the XSDT, finds the MADT inside
 * the XSDT, and counts the Local APIC entries the MADT reports - the
 * CPU count SMP bring-up needs. See acpi.h for the table structures.
 *
 * Why no checksum validation (yet): every table we read came from
 * QEMU's OVMF/SeaBIOS, which doesn't produce malformed ACPI tables.
 * A broken checksum here would mean reading uninitialised memory,
 * which the signature check already rules out - a stronger check
 * than it looks.
 */
#include <stdint.h>
#include <stddef.h>
#include "acpi.h"
#include "debug.h"
#include "string.h"

static int signatureMatches(const void *address, const char *signature, uint64_t length) {
    return memcmp(address, signature, length) == 0;
}

/* Sum-to-zero (mod 256) is ACPI's integrity check on every table. On
 * the RSDP it covers two spans: the first 20 bytes for the ACPI 1.0
 * part (`checksum` field), and the full structure `length` bytes for
 * the 2.0+ part (`extendedChecksum` field). The spec insists both be
 * 0 for the structure to be valid. */
static int bytesSumToZero(const void *address, uint64_t length) {
    const uint8_t *bytes = (const uint8_t *)address;
    uint8_t sum = 0;
    for (uint64_t i = 0; i < length; i++) {
        sum = (uint8_t)(sum + bytes[i]);
    }
    return sum == 0;
}

/* A signature match alone isn't enough - a stale "RSD PTR " string
 * left over in the EBDA will trigger it. The 20-byte checksum weeds
 * those out; a 2.0+ RSDP also needs its extended checksum to clear. */
static int rsdpIsValid(const Rsdp *candidate) {
    if (!bytesSumToZero(candidate, 20)) {
        return 0;
    }
    if (candidate->revision >= 2) {
        return bytesSumToZero(candidate, candidate->length);
    }
    return 1;
}

/* Walks [start, end) on 16-byte boundaries (the alignment the ACPI
 * spec guarantees for the RSDP) looking for a valid RSDP. Prefers an
 * ACPI 2.0+ structure (for its XSDT) over a 1.0-only one, so a 1.0
 * leftover in the EBDA doesn't shadow the real one in the BIOS area. */
static const Rsdp *scanForRsdp(uint64_t start, uint64_t end, int preferV2Only) {
    const Rsdp *v1Fallback = NULL;
    for (uint64_t address = start; address + sizeof(Rsdp) <= end; address += 16) {
        const Rsdp *candidate = (const Rsdp *)address;
        if (!signatureMatches(candidate->signature, RSDP_SIGNATURE, 8)) {
            continue;
        }
        if (!rsdpIsValid(candidate)) {
            continue;
        }
        if (candidate->revision >= 2) {
            return candidate;
        }
        if (!preferV2Only && !v1Fallback) {
            v1Fallback = candidate;
        }
    }
    return v1Fallback;
}

/* Two legal locations: the first 1KB of the EBDA (whose segment the
 * BDA stores at 0x40E), and the 0xE0000-0xFFFFF BIOS read-only range.
 * Prefer the 2.0+ RSDP found anywhere over the 1.0 one found first -
 * SeaBIOS/OVMF put the authoritative 2.0+ copy in the BIOS area, and
 * a pre-existing 1.0 stub in the EBDA would shadow it otherwise. */
static const Rsdp *findRsdp(void) {
    uint16_t ebdaSegment = *(volatile uint16_t *)EBDA_SEGMENT_PTR;
    uint64_t ebdaBase = (uint64_t)ebdaSegment << 4;

    const Rsdp *v2 = scanForRsdp(ebdaBase, ebdaBase + EBDA_SCAN_SIZE, 1);
    if (v2) {
        return v2;
    }
    v2 = scanForRsdp(BIOS_AREA_START, BIOS_AREA_END, 1);
    if (v2) {
        return v2;
    }
    const Rsdp *v1 = scanForRsdp(ebdaBase, ebdaBase + EBDA_SCAN_SIZE, 0);
    if (v1) {
        return v1;
    }
    return scanForRsdp(BIOS_AREA_START, BIOS_AREA_END, 0);
}

/* Walks an XSDT's or RSDT's entry array looking for a given signature.
 * The two differ only in entry width, so one helper reads the pointer
 * out, the other two decide where it comes from. */
static const AcpiTableHeader *findInXsdt(const Xsdt *xsdt, const char *signature) {
    uint64_t headerSize = sizeof(AcpiTableHeader);
    uint64_t entryCount = (xsdt->header.length - headerSize) / sizeof(uint64_t);
    for (uint64_t i = 0; i < entryCount; i++) {
        const AcpiTableHeader *table = (const AcpiTableHeader *)xsdt->entries[i];
        if (signatureMatches(table->signature, signature, 4)) {
            return table;
        }
    }
    return NULL;
}

static const AcpiTableHeader *findInRsdt(const Rsdt *rsdt, const char *signature) {
    uint64_t headerSize = sizeof(AcpiTableHeader);
    uint64_t entryCount = (rsdt->header.length - headerSize) / sizeof(uint32_t);
    for (uint64_t i = 0; i < entryCount; i++) {
        const AcpiTableHeader *table = (const AcpiTableHeader *)(uint64_t)rsdt->entries[i];
        if (signatureMatches(table->signature, signature, 4)) {
            return table;
        }
    }
    return NULL;
}

/* Counts the CPUs the MADT enumerates - both legacy Local APIC (type 0)
 * entries and x2APIC (type 9) entries. The "online capable" flag is
 * treated as "present and wake-able", same as "enabled", because a CPU
 * we have to bring up ourselves will initially report that way. */
static int countCpusInMadt(const Madt *madt) {
    int count = 0;
    uint64_t entriesSize = madt->header.length - sizeof(Madt);
    const uint8_t *cursor = madt->entries;
    const uint8_t *end = cursor + entriesSize;

    while (cursor < end) {
        const MadtEntryHeader *entry = (const MadtEntryHeader *)cursor;
        if (entry->length == 0) {
            /* A zero-length entry would loop forever - defensive stop. */
            break;
        }

        if (entry->type == MADT_TYPE_LOCAL_APIC) {
            const MadtLocalApic *lapic = (const MadtLocalApic *)cursor;
            if (lapic->flags & (LAPIC_FLAG_ENABLED | LAPIC_FLAG_ONLINE_CAPABLE)) {
                count++;
            }
        } else if (entry->type == MADT_TYPE_LOCAL_X2APIC) {
            const MadtLocalX2Apic *x2 = (const MadtLocalX2Apic *)cursor;
            if (x2->flags & (LAPIC_FLAG_ENABLED | LAPIC_FLAG_ONLINE_CAPABLE)) {
                count++;
            }
        }

        cursor += entry->length;
    }

    return count;
}

int countCpus(void) {
    uint16_t ebdaSeg = *(volatile uint16_t *)EBDA_SEGMENT_PTR;
    logInfo("ACPI: EBDA segment at BDA[0x40E] = %x (base %x)\n",
            ebdaSeg, (uint32_t)ebdaSeg << 4);

    const Rsdp *rsdp = findRsdp();
    if (!rsdp) {
        logError("ACPI RSDP not found in EBDA or 0xE0000-0xFFFFF\n");
        return 0;
    }

    logInfo("ACPI: RSDP at %p, revision=%u, rsdt=%x, xsdt=%lx\n",
            rsdp, rsdp->revision, rsdp->rsdtAddress, rsdp->xsdtAddress);

    /* Prefer the XSDT (ACPI 2.0+, 64-bit entries) but fall through to
     * the RSDT if the firmware only advertised one - QEMU/SeaBIOS can
     * still ship a revision-0 RSDP in some configurations. */
    const AcpiTableHeader *madtHeader = NULL;
    if (rsdp->revision >= 2 && rsdp->xsdtAddress != 0) {
        const Xsdt *xsdt = (const Xsdt *)rsdp->xsdtAddress;
        if (!signatureMatches(xsdt->header.signature, XSDT_SIGNATURE, 4)) {
            logError("XSDT at %lx has wrong signature\n", rsdp->xsdtAddress);
            return 0;
        }
        madtHeader = findInXsdt(xsdt, MADT_SIGNATURE);
    } else if (rsdp->rsdtAddress != 0) {
        const Rsdt *rsdt = (const Rsdt *)(uint64_t)rsdp->rsdtAddress;
        if (!signatureMatches(rsdt->header.signature, RSDT_SIGNATURE, 4)) {
            logError("RSDT at %x has wrong signature\n", rsdp->rsdtAddress);
            return 0;
        }
        madtHeader = findInRsdt(rsdt, MADT_SIGNATURE);
    } else {
        logError("RSDP has neither RSDT nor XSDT - cannot continue\n");
        return 0;
    }

    if (!madtHeader) {
        logError("MADT not present in RSDT/XSDT - cannot count CPUs\n");
        return 0;
    }

    const Madt *madt = (const Madt *)madtHeader;
    int count = countCpusInMadt(madt);

    logInfo("ACPI: MADT at %p - %d CPU(s) detected\n", madt, count);
    return count;
}
