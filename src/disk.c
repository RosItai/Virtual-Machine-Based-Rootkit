/* disk.c
 * High-level disk I/O functions using BIOS int 0x13 extended read.
 * These functions wrap the BIOS trampoline (bios_read_disk) to read
 * sectors from disk into memory, allowing loading of Windows and other
 * guest OS images.
 */

#include <stdint.h>
#include <stddef.h>
#include "disk.h"
#include "memory.h"
#include "debug.h"

/* External function implemented in bios_call.asm */
extern int bios_read_disk(uint8_t drive, dap_t *dap_ptr);

/* Get pointer to low-memory DAP structure */
static dap_t *get_dap(void) {
    return (dap_t *)DAP_ADDRESS;
}

/* Initialize DAP structure for reading
 * Parameters:
 *   dap: Pointer to DAP structure
 *   sector: Starting LBA sector number
 *   count: Number of sectors to read
 *   buffer: Physical address of buffer (in low memory)
 */
static void setup_dap(dap_t *dap, uint64_t sector, uint16_t count, uint32_t buffer) {
    dap->size = sizeof(dap_t);
    dap->unused = 0;
    dap->amount = count;
    dap->offset = buffer & 0xFFFF;           /* low 16 bits of address */
    dap->segment = (buffer >> 4) & 0xFFFF;   /* segment = addr >> 4 for real mode es:offset */
    dap->sector = sector;
}

/* Read a single sector from disk using BIOS int 0x13
 *
 * This is a low-level function that reads exactly one 512-byte sector.
 * For reading multiple sectors, use disk_read_sectors() instead.
 *
 * Parameters:
 *   drive: Drive number (0x80 = first HDD, 0x81 = second, etc.)
 *   sector: LBA sector number to read
 *   buffer: Pointer to 512-byte buffer (must be in low memory for BIOS access)
 *
 * Returns: 0 on success, non-zero (BIOS error code) on failure
 */
int disk_read_sector(uint8_t drive, uint64_t sector, void *buffer) {
    dap_t *dap = get_dap();
    uint32_t phys_addr = (uint32_t)(uintptr_t)buffer;

    /* Verify buffer is in accessible memory range */
    if (phys_addr > 0x100000) {
        return 0xFF;  /* Error: buffer not in low memory */
    }

    setup_dap(dap, sector, 1, phys_addr);
    return bios_read_disk(drive, dap);
}

/* Read multiple sectors from disk
 *
 * Reads up to 65535 sectors in a single call. For larger transfers,
 * call this function multiple times.
 *
 * Parameters:
 *   drive: Drive number (0x80 = first HDD, etc.)
 *   start_sector: Starting LBA sector number
 *   num_sectors: Number of sectors to read (1-65535)
 *   buffer: Pointer to buffer for sector data (must be large enough)
 *
 * Returns: Number of sectors successfully read, 0 on error
 */
uint32_t disk_read_sectors(uint8_t drive, uint64_t start_sector, uint32_t num_sectors, void *buffer) {
    dap_t *dap = get_dap();
    uint32_t phys_addr = (uint32_t)(uintptr_t)buffer;
    uint32_t sectors_read = 0;
    uint32_t remaining = num_sectors;
    uint64_t current_sector = start_sector;

    if (!buffer || num_sectors == 0) {
        return 0;
    }

    /* BIOS int 0x13 ah=0x42 can read at most 65535 sectors at once */
    while (remaining > 0) {
        uint16_t to_read = (remaining > MAX_BIOS_SECTORS) ? MAX_BIOS_SECTORS : remaining;

        setup_dap(dap, current_sector, to_read, phys_addr);

        int result = bios_read_disk(drive, dap);

        if (result != 0) {
            /* BIOS error occurred */
            if (sectors_read == 0) {
                return 0;  /* Couldn't read even the first sector */
            }
            break;  /* Return what we've read so far */
        }

        sectors_read += to_read;
        remaining -= to_read;
        current_sector += to_read;
        phys_addr += (to_read * SECTOR_SIZE);
    }

    return sectors_read;
}

/* Load entire disk image into memory
 *
 * Allocates a buffer and reads sectors from the disk.
 * Maximum image size is determined by max_sectors parameter.
 *
 * Parameters:
 *   drive: Drive number to read from (0x80 = first HDD)
 *   max_sectors: Maximum number of sectors to read
 *
 * Returns: Pointer to allocated buffer with disk contents, NULL on error
 */
void *disk_load_image(uint8_t drive, uint32_t max_sectors) {
    uint32_t image_size = max_sectors * SECTOR_SIZE;
    void *image_buffer = NULL;
    uint32_t sectors_read = 0;

    if (max_sectors == 0) {
        return NULL;
    }

    /* Allocate memory for the disk image */
    image_buffer = allocateMemory(image_size);
    if (!image_buffer) {
        return NULL;
    }

    /* Read sectors from disk into the buffer */
    sectors_read = disk_read_sectors(drive, 0, max_sectors, image_buffer);

    if (sectors_read == 0) {
        return NULL;  /* Failed to read any sectors */
    }

    return image_buffer;
}

/* Check if a drive exists by attempting to read its first sector
 *
 * Parameters:
 *   drive: Drive number to check
 *
 * Returns: 1 if drive exists and is readable, 0 otherwise
 */
int disk_drive_exists(uint8_t drive) {
    dap_t *dap = get_dap();

    /* Try to read just the first sector from the drive */
    setup_dap(dap, 0, 1, DISK_BUFFER_ADDRESS);

    int result = bios_read_disk(drive, dap);

    return (result == 0) ? 1 : 0;
}

/* Alternative: stream sectors with callback function
 * Useful for processing each sector as it arrives instead of buffering all.
 */
typedef void (*disk_sector_callback_t)(const void *sector_data, uint32_t sector_num, void *user_data);

uint32_t disk_read_sectors_streaming(uint8_t drive, uint64_t start_sector, uint32_t num_sectors,
                                      disk_sector_callback_t callback, void *user_data) {
    uint32_t sectors_read = 0;
    uint32_t i;

    if (!callback || num_sectors == 0) {
        return 0;
    }

    /* Read sectors one at a time and call callback for each */
    for (i = 0; i < num_sectors; i++) {
        int result = disk_read_sector(drive, start_sector + i, (void *)DISK_BUFFER_ADDRESS);

        if (result != 0) {
            break;  /* Error reading this sector */
        }

        callback((void *)DISK_BUFFER_ADDRESS, sectors_read, user_data);
        sectors_read++;
    }

    return sectors_read;
}

/* Read and verify boot sector signature
 * Windows boot sectors typically have 0xAA55 signature at offset 0x1FE
 */
int disk_verify_boot_sector(uint8_t drive, void *boot_sector) {
    int result = disk_read_sector(drive, 0, boot_sector);

    if (result != 0) {
        return 0;  /* Failed to read */
    }

    /* Check for boot signature (0xAA55) at offset 0x1FE */
    uint8_t *bs = (uint8_t *)boot_sector;
    uint16_t signature = (bs[0x1FF] << 8) | bs[0x1FE];

    return (signature == 0xAA55) ? 1 : 0;
}
