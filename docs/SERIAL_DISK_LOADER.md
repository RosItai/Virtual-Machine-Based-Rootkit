# Serial Disk Loader - Loading Windows & Other OS Images

This document describes the serial disk loader subsystem for VMBR, which allows loading disk images (such as Windows) into guest memory via serial port communication.

## Overview

The serial disk loader consists of two main components:

1. **Guest-side driver** (`src/serial_disk.c`, `include/serial.h`) - Runs on the VMBR hypervisor
2. **Host-side tool** (`tools/serial_disk_loader.c`) - Runs on the host machine

The host sends disk sectors via serial port, and the guest receives and stores them in allocated memory for use by the loaded OS.

## Architecture

### Guest-side Components

#### `include/serial.h`
Defines the interface for serial I/O and disk loading:

- `serial_init()` - Initialize serial port (38400 baud, 8N1)
- `serial_putc(c)` - Send a character
- `serial_print(s)` - Send a string
- `serial_getc()` - Receive a character (blocking)
- `serial_getc_timeout(timeout)` - Receive with timeout
- `serial_read_bytes(buf, len)` - Read multiple bytes
- `serial_write_bytes(buf, len)` - Write multiple bytes
- `serial_load_sectors(dest, max_sectors)` - Load disk sectors from host
- `serial_load_disk_image(out_sectors)` - Load full disk image into allocated memory
- `serial_send_sync()` - Send sync byte to host
- `serial_wait_handshake(timeout)` - Wait for host acknowledgment

#### `src/serial_disk.c`
Implementation of the guest-side protocol handler:

- Handles the protocol exchange with the host
- Manages sector buffering and delivery
- Provides both buffered (full image) and streaming (per-sector callback) loading modes
- Uses the memory allocator to allocate buffer space for disk images

### Host-side Components

#### `tools/serial_disk_loader.c`
Command-line utility for sending disk images:

```bash
./serial_disk_loader <serial_port> <image_file>
```

Options:
- `-v, --verbose` - Show detailed progress
- `-h, --help` - Display help message

Examples:
```bash
./serial_disk_loader /dev/ttyS0 windows.img
./serial_disk_loader /dev/ttyUSB0 ubuntu.iso
./serial_disk_loader COM3 disk.img  # Windows
```

## Protocol Specification

### Protocol Flow

The serial disk loader uses a simple request-response protocol:

```
GUEST                           HOST
  |                              |
  |-- Send 0xAA (PROTO_SYNC) --->|  (Ready for sector)
  |                              |
  |<-- Send 0x55 (PROTO_ACK) ---|  (Send sector) or 0xEE (PROTO_NACK, skip)
  |                              |
  |<-- Send 512 bytes (sector)---|  (If PROTO_ACK sent)
  |                              |
  |-- Send 0x55 (PROTO_ACK) ---->|  (Sector received)
  |                              |
  |-- Send 0xAA (PROTO_SYNC) --->|  (Ready for next)
  |                              |
  |<-- Send 0xFF (PROTO_END) ---|  (Transfer complete)
```

### Protocol Constants

```c
PROTO_SYNC  = 0xAA  /* Guest ready for sector */
PROTO_END   = 0xFF  /* Host signals end of transfer */
PROTO_ACK   = 0x55  /* Acknowledgment */
PROTO_NACK  = 0xEE  /* Negative acknowledgment */
SECTOR_SIZE = 512   /* Standard disk sector size */
```

## Usage Guide

### Building the Host Tool

```bash
cd tools
gcc -o serial_disk_loader serial_disk_loader.c

# On Windows (using MinGW or Visual Studio):
gcc -o serial_disk_loader.exe serial_disk_loader.c
```

### Loading a Disk Image

#### Step 1: Configure QEMU to Use Serial Port

Run QEMU with serial port redirection to the host's serial port:

```bash
qemu-system-x86_64 \
    -drive file=windows.img,format=raw \
    -serial /dev/ttyS0 \
    -m 4G \
    -enable-kvm
```

Or redirect to a named pipe (Linux):

```bash
mkfifo /tmp/serial_pipe
qemu-system-x86_64 \
    -drive file=windows.img,format=raw \
    -serial pipe:/tmp/serial_pipe \
    -m 4G \
    -enable-kvm
```

#### Step 2: Call `serial_load_disk_image()` from Guest Code

In your VMBR kernel initialization code:

```c
#include "serial.h"

void main_kernel(void) {
    serial_init();
    
    uint32_t sectors_loaded;
    void *image_buffer = serial_load_disk_image(&sectors_loaded);
    
    if (image_buffer && sectors_loaded > 0) {
        printf("Successfully loaded %u sectors from disk\n", sectors_loaded);
        // Now you can boot the guest OS from image_buffer
        launch_guest_os(image_buffer);
    } else {
        printf("Failed to load disk image\n");
    }
}
```

#### Step 3: Run the Host Loader

On the host machine:

```bash
./serial_disk_loader /dev/ttyS0 windows.img -v
```

The tool will:
1. Wait for the guest to send PROTO_SYNC
2. Respond with PROTO_ACK and the first sector
3. Wait for guest acknowledgment
4. Repeat until all sectors are transferred
5. Send PROTO_END to signal completion

### Streaming Load (Alternative)

For large images or when memory allocation is limited, use the streaming mode:

```c
void my_sector_callback(const uint8_t *sector_data, uint32_t sector_num, void *user_data) {
    // Process each sector as it arrives
    // e.g., write to disk, decompress, decrypt, etc.
    printf("Received sector %u\n", sector_num);
}

uint32_t sectors = serial_load_sectors_streaming(my_sector_callback, NULL);
```

## Implementation Details

### Memory Allocation

The disk image is allocated using the existing `allocateMemory()` function:

```c
/* Allocates memory for disk image (default: 256 MB max) */
void *serial_load_disk_image(uint32_t *out_sectors_loaded) {
    #define MAX_IMAGE_SIZE (256 * 1024 * 1024)
    #define MAX_SECTORS (MAX_IMAGE_SIZE / SECTOR_SIZE)
    
    uint8_t *image_buffer = (uint8_t *)allocateMemory(MAX_IMAGE_SIZE);
    // ...
}
```

You can adjust `MAX_IMAGE_SIZE` for larger disk images.

### Error Handling

The protocol includes error recovery:

- **Timeout**: If no response within timeout, transfer stops
- **NACK**: If host sends NACK, sector is skipped and next is attempted
- **Partial sectors**: If less than 512 bytes received, transfer stops

### Baud Rate

Default configuration: **38400 baud**

Configure both guest and host to use the same rate. To change:

**Guest side** (`src/serial_disk.c`):
```c
/* In serial_init() - modify divisor calculation */
outb(COM1 + 0, 0x03);  /* Divisor low byte (adjust for baud rate) */
```

**Host side** (`tools/serial_disk_loader.c`):
```c
#define BAUD_RATE 38400  /* Change this value */
```

## Troubleshooting

### "Timeout waiting for PROTO_SYNC"
- Guest code is not calling `serial_load_disk_image()` or `serial_load_sectors()`
- Serial port connection is not established
- Guest is halted or crashed before sending sync byte

### "No acknowledgment from guest"
- Timeout receiving guest's ACK byte
- Guest crashed during sector reception
- Serial port data corruption

### Transfer is very slow
- Baud rate is too low; increase to 115200 or higher if supported
- Serial connection has latency; check cable quality
- CPU bottleneck on host or guest

### Partial sectors transferred
- File is smaller than expected number of sectors
- Guest ran out of memory and failed allocation
- Timeout during transfer

## Performance Tuning

### Increasing Baud Rate

For faster transfers, increase baud rate. At 38400 baud:
- ~3.8 KB/s theoretical maximum
- 1 GB image takes ~4.5 minutes

At 115200 baud:
- ~11.5 KB/s theoretical maximum
- 1 GB image takes ~90 seconds

### Sector Buffering

For very large images (>256 MB), consider:
1. Streaming mode (process sectors as they arrive)
2. Splitting the image into multiple transfers
3. Using intermediate storage on the guest

## Example Integration

### Windows Boot Loader

```c
#include "serial.h"
#include "vmx.h"

typedef struct {
    uint8_t boot_signature[2];
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sectors;
    // ... rest of BIOS Parameter Block
} __attribute__((packed)) BootSector;

void load_and_boot_windows(void) {
    serial_init();
    
    printf("Loading Windows image from serial...\n");
    uint32_t sectors;
    void *boot_image = serial_load_disk_image(&sectors);
    
    if (!boot_image) {
        printf("ERROR: Failed to load Windows image\n");
        return;
    }
    
    printf("Loaded %u sectors (%u MB)\n", 
           sectors, (sectors * 512) / (1024*1024));
    
    /* Verify it looks like a Windows boot image */
    BootSector *bs = (BootSector *)boot_image;
    if (bs->boot_signature[0] == 0x55 && bs->boot_signature[1] == 0xAA) {
        printf("Boot sector signature valid\n");
        
        /* Setup guest memory and launch */
        setup_guest_memory(boot_image, sectors * 512);
        initialize_vmx();
        run_guest();
    } else {
        printf("ERROR: Invalid boot signature\n");
    }
}
```

## See Also

- `include/serial.h` - Serial driver API
- `src/serial_disk.c` - Guest-side implementation
- `tools/serial_disk_loader.c` - Host-side tool
- `src/kernel.c` - Existing serial port initialization

## Security Considerations

⚠️ **Warning**: The serial disk loader protocol assumes a **trusted host**.

The current implementation:
- Does NOT validate sector checksums
- Does NOT authenticate the host
- Does NOT verify image integrity

For untrusted environments, add:
- CRC32/SHA256 checksums per sector
- Signature verification
- Image authentication

## Future Enhancements

Potential improvements:
- DMA transfers (if hardware supports)
- Compression support (gzip, etc.)
- Image authentication/signature verification
- Parallel sector transfers
- Resume on interrupted transfer
- Progress indication protocol
- Multiple image support in single session
