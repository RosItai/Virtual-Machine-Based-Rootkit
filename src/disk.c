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
#include "string.h"

/* External function implemented in bios_call.asm */
extern int bios_read_disk(uint8_t drive, dap_t *dap_ptr);

/* Get pointer to low-memory DAP structure */
static dap_t *get_dap(void) {
    return (dap_t *)DAP_ADDRESS;
}

/* Initialize the disk driver and announce it over serial.
 * Prints a section header plus a success line, and probes the first
 * HDD so the banner also reports whether a readable drive is present. */
void disk_init(void) {
    logSection("Disk driver (BIOS int 0x13)");
    logInfo("Disk driver built successfully - DAP at %p, buffer at %p\n",
            (void *)DAP_ADDRESS, (void *)DISK_BUFFER_ADDRESS);

    /* Probe drive 0x80 with ONE BIOS int 0x13 call.  After it returns,
     * the bounce buffer (DISK_BUFFER_ADDRESS) already holds sector 0 -
     * read the signature from there directly, no second BIOS call and no
     * allocateMemory() (which isn't safe this early, before host paging
     * is set up by initializeHostPaging). */
    if (!disk_drive_exists(DRIVE_FIRST_HDD)) {
        logError("First HDD (0x%x) not found or not readable\n", DRIVE_FIRST_HDD);
        return;
    }

    /* The bounce buffer holds the sector data disk_drive_exists() read. */
    const uint8_t *sector0 = (const uint8_t *)DISK_BUFFER_ADDRESS;
    uint16_t signature = (uint16_t)((uint16_t)sector0[0x1FF] << 8) | sector0[0x1FE];
    logInfo("First HDD (0x%x) detected - signature 0x%x %s\n",
            DRIVE_FIRST_HDD, signature,
            (signature == 0xAA55) ? "(valid boot sector)" : "(no boot signature)");
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
    /* Real-mode segment:offset resolves to physical (segment*16 + offset).
     * To land exactly on `buffer`, put the top bits in segment and keep
     * offset to the low nibble - NOT offset = buffer & 0xFFFF, which would
     * add the low 16 bits twice (segment*16 already covers them) and point
     * the BIOS at the wrong address. */
    dap->segment = (buffer >> 4) & 0xFFFF;   /* paragraph number */
    dap->offset  = buffer & 0xF;             /* 0-15 within that paragraph */
    dap->sector = sector;
}

/* Read a single sector from disk using BIOS int 0x13.
 *
 * Reads exactly one 512-byte sector. The destination may be in high
 * memory - this routes through disk_read_sectors(), which bounces the
 * data through a low-memory buffer so real-mode BIOS can reach it.
 *
 * Parameters:
 *   drive: Drive number (0x80 = first HDD, 0x81 = second, etc.)
 *   sector: LBA sector number to read
 *   buffer: Pointer to a 512-byte buffer (high or low memory)
 *
 * Returns: 0 on success, non-zero on failure
 */
int disk_read_sector(uint8_t drive, uint64_t sector, void *buffer) {
    return (disk_read_sectors(drive, sector, 1, buffer) == 1) ? 0 : 1;
}

/* Read multiple sectors from disk, into a destination of any address.
 *
 * The destination buffer is usually in high memory (above 1MB, from
 * allocateMemory), but real-mode BIOS int 0x13 can only write below 1MB.
 * So every chunk is read first into a fixed low-memory bounce buffer
 * (DISK_BUFFER_ADDRESS, under 1MB) and then memcpy'd up to the real
 * destination. The bounce buffer is 64KB and 64KB-aligned (0x10000), so
 * a full chunk never crosses the 64KB boundary a BIOS DMA read must stay
 * within - that caps each BIOS read at DISK_BUFFER_SIZE / 512 sectors.
 *
 * Parameters:
 *   drive: Drive number (0x80 = first HDD, etc.)
 *   start_sector: Starting LBA sector number
 *   num_sectors: Number of sectors to read
 *   buffer: Destination for sector data (high or low memory), must hold
 *           num_sectors * 512 bytes
 *
 * Returns: Number of sectors successfully read, 0 on error
 */
uint32_t disk_read_sectors(uint8_t drive, uint64_t start_sector, uint32_t num_sectors, void *buffer) {
    dap_t *dap = get_dap();
    uint8_t *dest = (uint8_t *)buffer;
    uint32_t sectors_read = 0;
    uint32_t remaining = num_sectors;
    uint64_t current_sector = start_sector;

    /* Sectors that fit in the low bounce buffer in one BIOS read. */
    const uint32_t chunk_max = DISK_BUFFER_SIZE / SECTOR_SIZE;

    if (!buffer || num_sectors == 0) {
        return 0;
    }

    while (remaining > 0) {
        uint16_t to_read = (remaining > chunk_max) ? (uint16_t)chunk_max : (uint16_t)remaining;

        /* Read this chunk into the low bounce buffer. */
        setup_dap(dap, current_sector, to_read, DISK_BUFFER_ADDRESS);

        int result = bios_read_disk(drive, dap);

        if (result != 0) {
            /* BIOS error occurred */
            if (sectors_read == 0) {
                return 0;  /* Couldn't read even the first chunk */
            }
            break;  /* Return what we've read so far */
        }

        /* Copy the chunk up to the caller's (possibly high) destination. */
        memcpy(dest, (const void *)DISK_BUFFER_ADDRESS, (uint32_t)to_read * SECTOR_SIZE);

        sectors_read += to_read;
        remaining -= to_read;
        current_sector += to_read;
        dest += (uint32_t)to_read * SECTOR_SIZE;
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

/* Scan the BIOS fixed disks for the first bootable one and load its MBR
 * to 0x7C00 - the disk half of booting a guest OS.
 *
 * Mirrors the classic bootloader loop: walk drives 0x80..0xFF, read each
 * one's first sector, and take the first whose sector ends in the 0xAA55
 * signature. The winning sector is left at MBR_LOAD_ADDRESS (0x7C00),
 * exactly where a PC MBR expects to find itself, so whoever starts the
 * guest just points it at 0x7C00 with DL = the returned drive.
 *
 * Returns the bootable drive number, or 0 if none was found. */
uint8_t disk_find_bootable_drive(void) {
    uint8_t *mbr = (uint8_t *)MBR_LOAD_ADDRESS;
    uint16_t drive;

    /* uint16_t counter so the 0xFF..0x100 wrap ends the loop cleanly. */
    for (drive = DRIVE_FIRST_HDD; drive <= 0xFF; drive++) {
        /* Reads via the bounce buffer, then lands the MBR at 0x7C00. */
        if (disk_read_sector((uint8_t)drive, 0, mbr) != 0) {
            continue;  /* drive absent or unreadable - try the next */
        }

        uint16_t signature = (uint16_t)(mbr[0x1FF] << 8) | mbr[0x1FE];
        if (signature == MBR_BOOT_SIGNATURE) {
            logInfo("Bootable disk at drive 0x%x - MBR loaded to 0x%x\n",
                    drive, MBR_LOAD_ADDRESS);
            return (uint8_t)drive;
        }
    }

    logError("No bootable disk found (no 0x%x signature on any drive)\n",
             MBR_BOOT_SIGNATURE);
    return 0;
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
