/* serial_disk.c
 * Serial port-based disk I/O operations for loading guest OS images
 * (e.g., Windows) into guest memory via serial protocol.
 *
 * This driver provides a protocol for transferring disk sectors over
 * the serial port, useful for hypervisor contexts where direct disk
 * access is not available but serial communication is.
 */

#include <stdint.h>
#include <stddef.h>
#include "serial.h"
#include "memory.h"
#include "debug.h"

/* COM1 serial port base address */
#define COM1 0x3F8

/* Serial port I/O helper functions */
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* Line Status Register (LSR) bit flags */
#define LSR_TRANSMITTER_EMPTY 0x20
#define LSR_DATA_READY        0x01

/* Protocol control bytes */
#define PROTO_SYNC            0xAA  /* Handshake: ready for sector */
#define PROTO_END             0xFF  /* End of transfer marker */
#define PROTO_ACK             0x55  /* Acknowledgment from host */
#define PROTO_NACK            0xEE  /* Negative acknowledgment */

/* Sector size (standard for most disk systems) */
#define SECTOR_SIZE           512

/* Maximum timeout loops for serial operations (prevents infinite hangs) */
#define DEFAULT_TIMEOUT       10000000UL

/* Returns 1 if transmit buffer is empty (ready for more data) */
static int serial_transmit_empty(void) {
    return inb(COM1 + 5) & LSR_TRANSMITTER_EMPTY;
}

/* Returns 1 if data is ready in receive buffer */
static int serial_data_ready(void) {
    return inb(COM1 + 5) & LSR_DATA_READY;
}

/* Send a single byte with blocking wait */
void serial_send_byte(uint8_t byte) {
    while (!serial_transmit_empty());
    outb(COM1, byte);
}

/* Receive a single byte (blocking) */
uint8_t serial_recv_byte(void) {
    while (!serial_data_ready());
    return inb(COM1);
}

/* Receive a character with timeout.
 * Returns the character, or -1 if timeout occurs.
 */
int serial_getc_timeout(unsigned long timeout_loops) {
    unsigned long idle = 0;

    while (idle < timeout_loops) {
        if (serial_data_ready()) {
            return (int)(uint8_t)inb(COM1);
        }
        idle++;
    }

    return -1;  /* Timeout */
}

/* Receive a character (blocking indefinitely) */
uint8_t serial_getc(void) {
    return serial_recv_byte();
}

/* Read exactly `len` bytes from serial into buffer.
 * Returns number of bytes actually read (< len if timeout).
 */
uint32_t serial_read_bytes(uint8_t *buffer, uint32_t len) {
    uint32_t i = 0;

    for (i = 0; i < len; i++) {
        int byte = serial_getc_timeout(DEFAULT_TIMEOUT);
        if (byte < 0) {
            /* Timeout occurred */
            return i;
        }
        buffer[i] = (uint8_t)byte;
    }

    return i;
}

/* Write exactly `len` bytes from buffer to serial */
void serial_write_bytes(const uint8_t *buffer, uint32_t len) {
    uint32_t i;

    for (i = 0; i < len; i++) {
        serial_send_byte(buffer[i]);
    }
}

/* Send synchronization byte to indicate readiness */
void serial_send_sync(void) {
    serial_send_byte(PROTO_SYNC);
}

/* Wait for handshake from host (expects PROTO_ACK)
 * Returns 1 on success, 0 on timeout.
 */
int serial_wait_handshake(unsigned long timeout_loops) {
    int byte = serial_getc_timeout(timeout_loops);
    return (byte == PROTO_ACK) ? 1 : 0;
}

/* Load disk sectors over serial protocol.
 *
 * Protocol exchange:
 *   1. We send PROTO_SYNC (0xAA) to signal readiness
 *   2. We receive PROTO_ACK (0x55) or PROTO_NACK (0xEE) from host
 *   3. If ACK, we receive 512-byte sector
 *   4. Repeat steps 1-3 until we receive PROTO_END (0xFF)
 *
 * Returns number of sectors successfully loaded.
 */
uint32_t serial_load_sectors(uint8_t *dest_buffer, uint32_t max_sectors) {
    uint32_t sectors_loaded = 0;
    uint32_t current_offset = 0;

    if (!dest_buffer || max_sectors == 0) {
        return 0;
    }

    while (sectors_loaded < max_sectors) {
        /* Signal readiness to host */
        serial_send_sync();

        /* Wait for response from host */
        int response = serial_getc_timeout(DEFAULT_TIMEOUT);

        if (response < 0) {
            /* Timeout waiting for host response */
            return sectors_loaded;
        }

        if (response == PROTO_END) {
            /* Host signaled end of transfer */
            return sectors_loaded;
        }

        if (response == PROTO_NACK) {
            /* Host sent NACK, skip this sector and continue */
            continue;
        }

        if (response == PROTO_ACK) {
            /* Host ready to send sector - receive 512 bytes */
            uint32_t bytes_read = serial_read_bytes(
                dest_buffer + current_offset,
                SECTOR_SIZE
            );

            if (bytes_read < SECTOR_SIZE) {
                /* Short read - timeout or error occurred */
                return sectors_loaded;
            }

            sectors_loaded++;
            current_offset += SECTOR_SIZE;

            /* Send acknowledgment of receipt */
            serial_send_byte(PROTO_ACK);

            continue;
        }

        /* Unknown response byte - could be garbage, skip and retry */
    }

    return sectors_loaded;
}

/* Load a full disk image from serial port.
 * Allocates memory and loads sectors until transfer completes.
 * Returns pointer to loaded image (NULL on error).
 */
void *serial_load_disk_image(uint32_t *out_sectors_loaded) {
    /* Maximum size: 256 MB (reasonable for a guest OS image) */
    #define MAX_IMAGE_SIZE (256 * 1024 * 1024)
    #define MAX_SECTORS (MAX_IMAGE_SIZE / SECTOR_SIZE)

    uint8_t *image_buffer = NULL;
    uint32_t sectors_loaded = 0;

    if (!out_sectors_loaded) {
        return NULL;
    }

    /* Allocate memory for the image */
    image_buffer = (uint8_t *)allocateMemory(MAX_IMAGE_SIZE);
    if (!image_buffer) {
        *out_sectors_loaded = 0;
        return NULL;
    }

    /* Send initial sync to host indicating readiness */
    serial_send_sync();

    /* Wait for host handshake */
    if (!serial_wait_handshake(DEFAULT_TIMEOUT)) {
        *out_sectors_loaded = 0;
        return image_buffer;  /* Return allocated buffer (empty) */
    }

    /* Load sectors from host */
    sectors_loaded = serial_load_sectors(image_buffer, MAX_SECTORS);

    *out_sectors_loaded = sectors_loaded;
    return image_buffer;

    #undef MAX_IMAGE_SIZE
    #undef MAX_SECTORS
}

/* Alternative: Stream sectors directly without buffering entire image.
 * Calls callback for each sector received.
 * Useful for loading directly into guest memory or other processing.
 */
typedef void (*sector_callback_t)(const uint8_t *sector_data, uint32_t sector_num, void *user_data);

uint32_t serial_load_sectors_streaming(sector_callback_t callback, void *user_data) {
    uint8_t sector_buffer[SECTOR_SIZE];
    uint32_t sectors_loaded = 0;

    if (!callback) {
        return 0;
    }

    while (1) {
        /* Signal readiness */
        serial_send_sync();

        /* Wait for host response */
        int response = serial_getc_timeout(DEFAULT_TIMEOUT);

        if (response < 0 || response == PROTO_END) {
            return sectors_loaded;
        }

        if (response == PROTO_NACK) {
            continue;
        }

        if (response == PROTO_ACK) {
            /* Receive sector */
            uint32_t bytes_read = serial_read_bytes(sector_buffer, SECTOR_SIZE);

            if (bytes_read < SECTOR_SIZE) {
                return sectors_loaded;
            }

            /* Call callback with received sector */
            callback(sector_buffer, sectors_loaded, user_data);

            /* Acknowledge receipt */
            serial_send_byte(PROTO_ACK);

            sectors_loaded++;
            continue;
        }
    }

    return sectors_loaded;
}
