/* disk.h - BIOS int 0x13 extended disk reading interface
 * Uses Disk Address Packet (DAP) to read sectors via BIOS calls.
 * This allows loading Windows and other OS images from disk into RAM.
 */

#ifndef DISK_H
#define DISK_H

#include <stdint.h>

/* Disk Address Packet (DAP) for extended read (int 0x13, ah=0x42)
 * This structure tells the BIOS which sectors to read and where to put them.
 * Must be in low memory (below 1MB) for BIOS access.
 */
typedef struct {
    uint8_t  size;              /* 0: Size of DAP (16 bytes) */
    uint8_t  unused;            /* 1: Reserved (must be 0) */
    uint16_t amount;            /* 2: Number of sectors to read */
    uint16_t offset;            /* 4: Offset of buffer (es:offset addressing) */
    uint16_t segment;           /* 6: Segment of buffer (es:offset addressing) */
    uint64_t sector;            /* 8: Starting LBA sector number */
} __attribute__((packed)) dap_t;

/* Physical addresses for DAP and disk operations in low memory
 * These must not conflict with other low-memory structures.
 * Currently using high end of reserved area: 0x600-0x7BFF is free.
 */
/* Low-memory layout these must NOT collide with (see memory_map.asm and
 * bios_call.asm):
 *   0x1000-0x1150  BIOS trampoline (the code running during int 0x13)
 *   0x2000-0x2504  BIOS E820 memory map
 *   0x7C00         real-mode stack (grows down)
 * The DAP goes in the free gap after the memory map; the sector buffer
 * goes at 0x10000 - a free 64KB-aligned region, well clear of all of the
 * above and still under 1MB so real-mode BIOS int 0x13 can reach it. */
#define DAP_ADDRESS          0x2600  /* DAP structure (free, after memory map) */
#define DISK_BUFFER_ADDRESS  0x10000 /* Temporary buffer for disk reads */
#define DISK_BUFFER_SIZE     (64 * 1024)  /* 64KB buffer for disk operations */

/* Initialize the disk driver and print a readiness banner over serial.
 * Probes the first HDD (0x80) so the message also reports whether a
 * readable drive is actually present. Safe to call once during boot. */
void disk_init(void);

/* Read a single sector from disk using BIOS int 0x13
 *
 * Parameters:
 *   drive: Drive number (0x80 = first HDD, 0x81 = second HDD, etc.)
 *   sector: LBA sector number to read
 *   buffer: Pointer to memory where sector should be stored (must be in low memory)
 *
 * Returns: 0 on success, non-zero on error
 */
int disk_read_sector(uint8_t drive, uint64_t sector, void *buffer);

/* Read multiple sectors from disk
 *
 * Parameters:
 *   drive: Drive number (0x80 = first HDD, etc.)
 *   start_sector: Starting LBA sector number
 *   num_sectors: Number of sectors to read
 *   buffer: Pointer to buffer for data (minimum size: num_sectors * 512)
 *
 * Returns: Number of sectors successfully read, 0 on error
 */
uint32_t disk_read_sectors(uint8_t drive, uint64_t start_sector, uint32_t num_sectors, void *buffer);

/* Load entire disk image into memory
 * Allocates memory and reads all sectors from the drive.
 *
 * Parameters:
 *   drive: Drive number (0x80 for first HDD)
 *   max_sectors: Maximum number of sectors to read
 *
 * Returns: Pointer to allocated buffer with disk contents, NULL on error
 */
void *disk_load_image(uint8_t drive, uint32_t max_sectors);

/* Get disk drive information
 *
 * Parameters:
 *   drive: Drive number to check
 *
 * Returns: 1 if drive exists, 0 if not
 */
int disk_drive_exists(uint8_t drive);

/* Standard drive numbers */
#define DRIVE_FIRST_HDD  0x80
#define DRIVE_SECOND_HDD 0x81
#define DRIVE_FIRST_FDD  0x00
#define DRIVE_SECOND_FDD 0x01

/* Standard sector size for HDDs */
#define SECTOR_SIZE 512

/* Sector addressing limits */
#define MAX_BIOS_SECTORS 0x10000  /* 64K sectors max per operation */

/* Where a disk's first sector (the MBR) is placed for boot, by the
 * long-standing BIOS convention every PC MBR - including Windows' -
 * assumes: physical 0x7C00, reached in real mode as 0x0000:0x7C00. */
#define MBR_LOAD_ADDRESS 0x7C00

/* The two-byte little-endian signature a bootable MBR ends with, at
 * offset 0x1FE: 0x55 then 0xAA (read as the word 0xAA55). */
#define MBR_BOOT_SIGNATURE 0xAA55

/* Scan the BIOS fixed-disk drives (0x80..0xFF) for the first one whose
 * first sector carries the 0xAA55 boot signature, and load that sector
 * (the MBR) to MBR_LOAD_ADDRESS (0x7C00). This is the disk half of
 * handing control to a guest OS: the caller still has to actually start
 * the guest at 0x7C00 with DL set to the returned drive.
 *
 * Returns the drive number (0x80..0xFF) of the bootable disk, or 0 if no
 * drive had a valid boot signature. */
uint8_t disk_find_bootable_drive(void);

#endif
