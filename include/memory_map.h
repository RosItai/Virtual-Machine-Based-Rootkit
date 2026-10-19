/* memory_map.h
 * C-side view of the BIOS E820 memory map that bios_call.asm's
 * bios_load_memory_map leaves in low memory. MEMORY_MAP_TABLE_ADDR and
 * the struct layout below must stay in sync with
 * src/boot/memory_map.asm (MEMORY_MAP_COUNT_ADDR/MEMORY_MAP_ENTRIES_ADDR/
 * MEMORY_MAP_ENTRY_SIZE) - there's no single shared source of truth
 * between the NASM and C sides, so a change to one needs the other
 * updated by hand.
 */
#ifndef MEMORY_MAP_H
#define MEMORY_MAP_H

#include <stdint.h>

#define MEMORY_MAP_TABLE_ADDR 0x2000

/* 20 bytes: no extendedAttributes field - see the MEMORY_MAP_ENTRY_SIZE
 * comment in src/boot/memory_map.asm for why. Must stay in sync with
 * that file if it ever changes back. */
typedef struct {
    uint64_t baseAddress;
    uint64_t length;
    uint32_t type;
} __attribute__((packed)) MemoryMapEntry;

/* numberOfEntries sits at MEMORY_MAP_TABLE_ADDR (0x2000), and entries[]
 * starts immediately after it (0x2004) - a flexible array member here
 * matches that layout exactly, so a MemoryMapTable* aimed straight at
 * MEMORY_MAP_TABLE_ADDR reads both without any translation. */
typedef struct {
    uint32_t numberOfEntries;
    MemoryMapEntry entries[];
} __attribute__((packed)) MemoryMapTable;

#define E820_USABLE           1
#define E820_RESERVED         2
#define E820_ACPI_RECLAIMABLE 3
#define E820_ACPI_NVS         4
#define E820_BAD              5

#endif
