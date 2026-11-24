/* acpi.h
 * Minimal ACPI table parsing - just enough to answer "how many CPUs
 * does this machine have?" by walking RSDP -> XSDT -> MADT and counting
 * enabled Local APIC entries. See acpi.c for the implementation.
 *
 * The table structures below are architectural (ACPI specification
 * Vol 1, chapters 5 and 6), not implementation-specific.
 */
#ifndef ACPI_H
#define ACPI_H

#include <stdint.h>

/* --- Where to look for the RSDP ---
 * ACPI spec 5.2.5.1: either the first 1KB of the EBDA, or the
 * 0xE0000-0xFFFFF BIOS read-only range. Both are below 1MB and already
 * identity-mapped. */
#define EBDA_SEGMENT_PTR 0x40E     /* uint16_t in the BDA: EBDA segment */
#define EBDA_SCAN_SIZE   1024u
#define BIOS_AREA_START  0xE0000ull
#define BIOS_AREA_END    0x100000ull

/* --- Signatures (first bytes of each table's header) --- */
#define RSDP_SIGNATURE "RSD PTR "  /* 8 bytes, trailing space is required */
#define RSDT_SIGNATURE "RSDT"      /* 4 bytes - 32-bit entries, ACPI 1.0 */
#define XSDT_SIGNATURE "XSDT"      /* 4 bytes - 64-bit entries, ACPI 2.0+ */
#define MADT_SIGNATURE "APIC"      /* 4 bytes - "Multiple APIC Description Table" */

/* --- MADT interrupt-controller-structure types --- */
#define MADT_TYPE_LOCAL_APIC  0u
#define MADT_TYPE_LOCAL_X2APIC 9u

/* --- Local APIC entry flags --- */
#define LAPIC_FLAG_ENABLED        (1u << 0)
#define LAPIC_FLAG_ONLINE_CAPABLE (1u << 1)

/* Root System Description Pointer. ACPI 2.0+ adds everything from
 * `length` onward; a revision-0 (ACPI 1.0) RSDP stops at rsdtAddress
 * and has no XSDT. */
typedef struct {
    char     signature[8];
    uint8_t  checksum;
    char     oemId[6];
    uint8_t  revision;
    uint32_t rsdtAddress;
    /* ACPI 2.0+ fields: */
    uint32_t length;
    uint64_t xsdtAddress;
    uint8_t  extendedChecksum;
    uint8_t  reserved[3];
} __attribute__((packed)) Rsdp;

/* The 36-byte header every ACPI table starts with. The table type is
 * identified by `signature`, and `length` is the total table size
 * including this header. */
typedef struct {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oemId[6];
    char     oemTableId[8];
    uint32_t oemRevision;
    uint32_t creatorId;
    uint32_t creatorRevision;
} __attribute__((packed)) AcpiTableHeader;

/* XSDT: ACPI table header followed by an array of 64-bit physical
 * addresses, one per other ACPI table in the system. */
typedef struct {
    AcpiTableHeader header;
    uint64_t        entries[];
} __attribute__((packed)) Xsdt;

/* RSDT: the ACPI 1.0 equivalent, with 32-bit entries. QEMU/SeaBIOS can
 * still ship a revision-0 RSDP that points only at an RSDT; we walk
 * it the same way, just with narrower pointers. */
typedef struct {
    AcpiTableHeader header;
    uint32_t        entries[];
} __attribute__((packed)) Rsdt;

/* MADT (ACPI spec 5.2.12): a header, the legacy LAPIC MMIO address,
 * some flags, then a variable-length run of interrupt-controller
 * structures. */
typedef struct {
    AcpiTableHeader header;
    uint32_t        localApicAddress;
    uint32_t        flags;
    uint8_t         entries[]; /* walk by (type, length) prefix */
} __attribute__((packed)) Madt;

/* The (type, length) prefix every MADT entry starts with. Length is
 * total entry size including this prefix. */
typedef struct {
    uint8_t type;
    uint8_t length;
} __attribute__((packed)) MadtEntryHeader;

/* Type 0: Processor Local APIC entry. One per logical CPU the
 * firmware reports, though not all are necessarily usable - the flags
 * field distinguishes enabled from merely online-capable. */
typedef struct {
    MadtEntryHeader header;
    uint8_t         processorId;
    uint8_t         apicId;
    uint32_t        flags;
} __attribute__((packed)) MadtLocalApic;

/* Type 9: Processor Local x2APIC entry. Same role as type 0 but with
 * a 32-bit APIC ID for CPUs beyond the 8-bit xAPIC limit. */
typedef struct {
    MadtEntryHeader header;
    uint16_t        reserved;
    uint32_t        x2ApicId;
    uint32_t        flags;
    uint32_t        processorUid;
} __attribute__((packed)) MadtLocalX2Apic;

/* Walks the firmware's ACPI tables and returns the number of enabled
 * (or online-capable) CPUs the MADT lists. Returns 0 if the tables
 * can't be found or parsed - a 1-CPU fallback is the caller's call. */
int countCpus(void);

#endif
