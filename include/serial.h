/* serial.h - COM1 serial port helpers and disk I/O operations */
#ifndef SERIAL_H
#define SERIAL_H

#include <stdint.h>

/* Basic serial port operations */
void serial_init(void);
void serial_putc(char c);
void serial_print(const char *s);
uint8_t serial_getc(void);

/* Serial-based disk operations for loading guest OS images */

/* Read a single character with timeout. Returns -1 on timeout. */
int serial_getc_timeout(unsigned long timeout_loops);

/* Read exactly `len` bytes from serial port into buffer.
 * Blocks until all bytes received. Returns number of bytes read (< len on timeout). */
uint32_t serial_read_bytes(uint8_t *buffer, uint32_t len);

/* Write exactly `len` bytes to serial port.
 * Blocks until all bytes transmitted. */
void serial_write_bytes(const uint8_t *buffer, uint32_t len);

/* Protocol: Load disk sectors over serial port.
 *
 * Protocol format:
 *   - Sends a sync byte (0xAA) to signal readiness for a sector
 *   - Receives 512-byte sector data from host
 *   - Repeats until end of transfer (marked by sync byte 0xFF)
 *
 * Returns number of sectors successfully loaded.
 */
uint32_t serial_load_sectors(uint8_t *dest_buffer, uint32_t max_sectors);

/* Load a full disk image from serial port into memory.
 * Allocates memory for the image and loads it via serial_load_sectors.
 * Returns pointer to allocated buffer (or NULL on error).
 */
void *serial_load_disk_image(uint32_t *out_sectors_loaded);

/* Send a synchronization pattern to host indicating readiness.
 * Used by the guest to signal it's ready to receive data. */
void serial_send_sync(void);

/* Receive handshake from host (waits for host-side protocol marker) */
int serial_wait_handshake(unsigned long timeout_loops);

#endif
