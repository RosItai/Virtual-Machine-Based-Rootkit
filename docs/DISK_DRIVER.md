# BIOS Disk Driver - Loading Windows via int 0x13

This document describes the BIOS disk driver for VMBR, which uses `int 0x13` (Extended Read Sectors) to load Windows and other guest OS images from disk into RAM.

This driver uses the **Disk Address Packet (DAP)** protocol, the same approach demonstrated in iteration 7 of this project.

## Overview

The disk driver consists of:

1. **Guest-side components** (in hypervisor):
   - `include/disk.h` - DAP structure and function declarations
   - `src/disk.c` - High-level disk I/O functions
   - `src/bios_call.asm` - Real-mode BIOS int 0x13 trampoline

2. **Integration points**:
   - Uses existing `bios_call.asm` infrastructure for mode switching
   - Works with memory allocator for image buffers
   - Integrates into standard Makefile build

## DAP (Disk Address Packet) Structure

The DAP tells BIOS where to read and what to read:

```c
typedef struct {
    uint8_t  size;              /* 0: Size of DAP (16 bytes) */
    uint8_t  unused;            /* 1: Reserved (must be 0) */
    uint16_t amount;            /* 2: Number of sectors to read */
    uint16_t offset;            /* 4: Offset of buffer (es:offset) */
    uint16_t segment;           /* 6: Segment of buffer (es:offset) */
    uint64_t sector;            /* 8: Starting LBA sector number */
} __attribute__((packed)) dap_t;
```

**Memory layout (es:offset addressing)**:
- `segment` = physical_address >> 4
- `offset` = physical_address & 0x0F

For example, to access physical address `0x10000`:
- `segment = 0x1000`
- `offset = 0x0000`

## BIOS int 0x13 Call Sequence

The driver uses BIOS int 0x13 with extended read (function 0x42):

```asm
mov ah, 0x42              ; Extended Read Sectors function
mov dl, drive_number      ; Drive number (0x80 = first HDD)
mov si, dap_offset        ; Offset to DAP structure
int 0x13                  ; Call BIOS
                          ; Result in AH: 0 = success, non-zero = error
                          ; CF set if error
```

## API Functions

### `int disk_read_sector(uint8_t drive, uint64_t sector, void *buffer)`

Read a single 512-byte sector from disk.

**Parameters:**
- `drive`: Drive number (0x80 for first HDD, 0x81 for second, etc.)
- `sector`: LBA sector number
- `buffer`: Pointer to 512-byte buffer (must be in memory accessible to BIOS, below 1MB)

**Returns:** 0 on success, non-zero (BIOS error code) on failure

**Example:**
```c
#include "disk.h"

uint8_t boot_sector[512];
int result = disk_read_sector(0x80, 0, boot_sector);  /* Read sector 0 */

if (result == 0) {
    printf("Successfully read boot sector\n");
} else {
    printf("Error reading boot sector: 0x%02x\n", result);
}
```

### `uint32_t disk_read_sectors(uint8_t drive, uint64_t start_sector, uint32_t num_sectors, void *buffer)`

Read multiple consecutive sectors.

**Parameters:**
- `drive`: Drive number
- `start_sector`: Starting LBA sector number
- `num_sectors`: Number of sectors to read (1-65535)
- `buffer`: Pointer to buffer (minimum size: num_sectors * 512 bytes)

**Returns:** Number of sectors successfully read, 0 on error

**Example:**
```c
uint32_t image_size = 1024 * 1024;  /* 1 MB */
uint32_t num_sectors = image_size / 512;

void *image_buffer = allocateMemory(image_size);
uint32_t sectors_read = disk_read_sectors(0x80, 0, num_sectors, image_buffer);

printf("Read %u sectors (%u bytes)\n", sectors_read, sectors_read * 512);
```

### `void *disk_load_image(uint8_t drive, uint32_t max_sectors)`

Allocate memory and load a complete disk image.

**Parameters:**
- `drive`: Drive number (0x80 for first HDD)
- `max_sectors`: Maximum number of sectors to read

**Returns:** Pointer to allocated buffer with disk contents, NULL on error

**Example:**
```c
#include "disk.h"

/* Load entire first HDD (up to 256 MB) */
#define MAX_IMAGE_SIZE (256 * 1024 * 1024)
#define MAX_SECTORS (MAX_IMAGE_SIZE / 512)

void *windows_image = disk_load_image(0x80, MAX_SECTORS);

if (windows_image) {
    printf("Successfully loaded Windows image\n");
    /* Boot guest OS from windows_image */
} else {
    printf("Failed to load Windows image\n");
}
```

### `int disk_drive_exists(uint8_t drive)`

Check if a drive is accessible.

**Parameters:**
- `drive`: Drive number to check

**Returns:** 1 if drive exists and readable, 0 otherwise

**Example:**
```c
if (disk_drive_exists(0x80)) {
    printf("First HDD is accessible\n");
} else {
    printf("First HDD not found\n");
}
```

### `int disk_verify_boot_sector(uint8_t drive, void *boot_sector)`

Read and verify the boot sector signature.

**Parameters:**
- `drive`: Drive number
- `boot_sector`: Pointer to 512-byte buffer for boot sector

**Returns:** 1 if valid boot sector (0xAA55 signature), 0 otherwise

**Example:**
```c
uint8_t boot[512];
if (disk_verify_boot_sector(0x80, boot)) {
    printf("Valid Windows boot sector found\n");
} else {
    printf("No valid boot sector\n");
}
```

## Integration with Existing Code

The disk driver integrates seamlessly with VMBR's existing infrastructure:

### Memory Allocation

Disk images are loaded using the existing `allocateMemory()` function:

```c
/* Allocate 256 MB for disk image */
void *image = allocateMemory(256 * 1024 * 1024);
```

### BIOS Call Infrastructure

The driver reuses the BIOS call trampoline mechanism from `bios_call.asm`:
- Mode switching (long mode → real mode → long mode)
- Low-memory IDT management
- Stack switching

### Build System

The driver is automatically compiled by the Makefile:
```bash
make build  # Compiles include/disk.h, src/disk.c, and bios_call.asm
```

## Usage: Loading Windows

### Step 1: Check Drive and Boot Sector

```c
#include "disk.h"

void main_kernel(void) {
    serial_init();
    
    /* Check if drive exists */
    if (!disk_drive_exists(0x80)) {
        printf("ERROR: First HDD not found\n");
        while(1);
    }
    
    /* Verify boot sector signature */
    uint8_t boot[512];
    if (!disk_verify_boot_sector(0x80, boot)) {
        printf("ERROR: No valid boot sector\n");
        while(1);
    }
    
    printf("Boot sector is valid\n");
}
```

### Step 2: Load Windows Image

```c
#include "disk.h"
#include "vmx.h"

void main_kernel(void) {
    serial_init();
    
    /* Load Windows (up to 1 GB) */
    uint32_t max_sectors = (1024 * 1024 * 1024) / 512;
    void *windows_image = disk_load_image(0x80, max_sectors);
    
    if (!windows_image) {
        printf("ERROR: Failed to load Windows\n");
        while(1);
    }
    
    printf("Successfully loaded Windows image\n");
    printf("Image size: %u sectors (%u MB)\n", 
           max_sectors, (max_sectors * 512) / (1024*1024));
    
    /* Setup guest memory and boot */
    setup_guest_memory(windows_image, max_sectors * 512);
    initialize_vmx();
    run_guest();
}
```

### Step 3: Boot Windows in Guest

The loaded Windows image is now in memory and ready to boot as a guest OS.

## QEMU Configuration

To test the disk driver with QEMU:

```bash
qemu-system-x86_64 \
    -drive file=windows.img,format=raw,index=0,media=disk \
    -m 4G \
    -enable-kvm \
    -cpu host \
    -cdrom build/vmbr.iso \
    -nographic
```

The `-drive` parameter provides the Windows image as the first HDD (drive 0x80).

## Low-Memory Buffer Management

The driver uses fixed low-memory addresses for BIOS communication:

```c
#define DAP_ADDRESS          0x7A00  /* DAP structure */
#define DISK_BUFFER_ADDRESS  0x1000  /* Temporary read buffer (64 KB) */
```

These addresses are:
- Below 1 MB (accessible to BIOS in real mode)
- Out of way of kernel code and stack
- Reused across reads (not permanently allocated)

## Error Codes

BIOS int 0x13 returns error codes in AH:

| Code | Meaning |
|------|---------|
| 0x00 | Success |
| 0x01 | Invalid function/parameter |
| 0x02 | Address mark not found |
| 0x03 | Disk write-protected |
| 0x04 | Sector not found |
| 0x05 | Reset failed |
| 0x09 | DMA overrun |
| 0x0C | Unsupported track/invalid media |
| 0x10 | CRC error on read |
| 0x20 | Controller failure |
| 0x40 | Seek failed |
| 0x80 | Timeout |
| 0xAA | Drive not ready |
| 0xBB | Undefined error |
| 0xCC | Write fault |
| 0xFF | Error: sense operation failed (or buffer error) |

## Performance Considerations

### Speed

At typical QEMU disk speeds:
- ~20-30 MB/second sustained throughput
- 1 GB Windows image: ~30-50 seconds

For faster transfers, consider:
1. Reducing disk image size (strip unnecessary drivers, tools)
2. Using compressed images and decompressing in guest
3. Enabling disk caching in QEMU

### Buffer Size Limits

- Maximum sectors per read: 65535 (limited by BIOS int 0x13)
- DAP buffer offset: 0-65535
- For larger transfers, use multiple `disk_read_sectors()` calls

## Troubleshooting

### "Drive not found" (error 0xAA)
- Verify QEMU `-drive` parameter
- Check drive number (0x80 = first HDD)
- Ensure Windows image file exists and is readable

### "Sector not found" (error 0x04)
- Verify sector number is valid
- Check image file size
- Ensure LBA mode is supported (most modern drives)

### "Invalid function" (error 0x01)
- BIOS may not support extended read (function 0x42)
- Older machines may only support function 0x02 (CHS read)
- Verify function 0x42 support in BIOS

### Timeout (error 0x80)
- Disk may be malfunctioning
- Try smaller transfers
- Check QEMU simulated disk status

## Comparison with Serial Loader

| Aspect | int 0x13 (This Driver) | Serial (Alternative) |
|--------|---------|--------|
| **Speed** | ~20-30 MB/s | ~4 KB/s (at 38400 baud) |
| **Hardware** | HDD/emulated disk | Serial port |
| **Complexity** | BIOS int 0x13 | Protocol overhead |
| **Windows Boot** | Standard boot | Custom protocol |
| **Real-world** | Actual drives | VM simulation |

The int 0x13 approach is better for:
- Standard Windows boot from disk
- Real hardware compatibility
- Performance-critical loading

## See Also

- `include/disk.h` - API definitions
- `src/disk.c` - Implementation
- `src/bios_call.asm` - BIOS call infrastructure
- Example iteration 7 code (your friend's work)

## References

- **BIOS Interrupt Reference**: int 0x13 extensions
- **LBA Mode**: Logical Block Addressing for disk sectors
- **DAP Format**: Standard for extended read operations
