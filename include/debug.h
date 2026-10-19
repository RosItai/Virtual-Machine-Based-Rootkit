/* debug.h - tiny printf for kernel debugging, writes to the COM1 serial port */
#ifndef DEBUG_H
#define DEBUG_H

/* %d signed, %u unsigned, %x hex, %p pointer, %c char, %s string, %% literal
 * %ld/%lu/%lx: same, but for "long" (64-bit) instead of "int" (32-bit) */
void printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Same format support as printf, prefixed with a colored "[INFO] "/
 * "[ERROR] " tag - use these instead of raw printf for status/error
 * messages so output stays visually consistent. */
void logInfo(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void logError(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Blank line, then a cyan "== title ==" header. Every section of boot
 * output starts with this, so spacing stays uniform. */
void logSection(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
