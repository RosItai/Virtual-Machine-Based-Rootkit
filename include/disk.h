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
#define DAP_ADDRESS          0x7A00  /* DAP structure location */
#define DISK_BUFFER_ADDRESS  0x1000  /* Temporary buffer for disk reads */
#define DISK_BUFFER_SIZE     (64 * 1024)  /* 64KB buffer for disk operations */

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

#endif
