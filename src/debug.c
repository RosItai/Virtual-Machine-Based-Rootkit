/* debug.c
 * A tiny printf for kernel debugging, writing to the same COM1 serial
 * port kernel.c already sets up. Supports:
 *   %d  signed decimal (int)      %u  unsigned decimal (unsigned int)
 *   %x  unsigned hex, no padding  %p  pointer: "0x" + zero-padded hex
 *   %c  single character          %s  null-terminated string
 *   %ld/%lu/%lx  same as d/u/x but for "long" (64-bit on this target)
 *                instead of "int" (32-bit) - required for uint64_t
 *                values (e.g. memory addresses/lengths): passing one
 *                to plain %x/%u would read only a 32-bit slot via
 *                va_arg while the caller actually passed 64 bits.
 *   %%  literal '%'
 * An unrecognized specifier prints the '%' as-is instead of being
 * silently swallowed, and %s prints directly instead of recursing
 * back into printf - a format string that happens to contain a
 * literal '%' inside %s data would otherwise read a variadic argument
 * that was never passed.
 */
#include <stdint.h>
#include <stdarg.h>
#include "serial.h"
#include "debug.h"
#include "colors.h"

/* VERBOSE_LOG gates the progress stream: logInfo and logSection drop to
 * no-ops when it's off so a release / demo run sees only the big ASCII
 * banner (printed via serial_print in kernel.c, not logInfo) plus any
 * actual error line. logError ALWAYS prints, because a silent BSOD or
 * triple-fault with no clue is the worst possible outcome. The guard
 * sits inside the public functions themselves, so flipping it just
 * rebuilds debug.o, not every .c file that calls logInfo. The Makefile's
 * -log targets flip it to 1 via EXTRA_CFLAGS. */
#ifndef VERBOSE_LOG
#define VERBOSE_LOG 0
#endif

/* Serial port is one global device, and once APs started calling logInfo
 * concurrently with the BSP (SMP bring-up in apBringUp) their messages
 * interleaved mid-line into garbage. One spinlock around each public
 * entry point keeps every emitted message atomic; the printing itself
 * never blocks so the critical section stays short. 0 = free, 1 = held. */
static volatile uint32_t logLock = 0;

static void acquireLog(void) {
    while (__sync_lock_test_and_set(&logLock, 1)) {
        __asm__ volatile("pause");
    }
}

static void releaseLog(void) {
    __sync_lock_release(&logLock);
}

static void print_unsigned(unsigned long value, unsigned base) {
    char digits[32];            /* enough for a 64-bit value in base 2 */
    int len = 0;

    if (value == 0) {
        digits[len++] = '0';
    } else {
        while (value != 0) {
            unsigned digit = value % base;
            digits[len++] = (digit < 10) ? ('0' + digit) : ('a' + digit - 10);
            value /= base;
        }
    }
    while (len > 0) {
        serial_putc(digits[--len]);
    }
}

static void print_pointer(const void *ptr) {
    uintptr_t value = (uintptr_t)ptr;
    serial_print("0x");
    for (int shift = (int)(sizeof(value) * 8) - 4; shift >= 0; shift -= 4) {
        unsigned digit = (unsigned)((value >> shift) & 0xF);
        serial_putc(digit < 10 ? ('0' + digit) : ('a' + digit - 10));
    }
}

static void vprintf_impl(const char *fmt, va_list args) {
    for (int i = 0; fmt[i]; i++) {
        if (fmt[i] != '%') {
            serial_putc(fmt[i]);
            continue;
        }
        int has_long = 0;
        int spec_offset = 1;
        if (fmt[i + 1] == 'l') {
            has_long = 1;
            spec_offset = 2;
        }
        switch (fmt[i + spec_offset]) {
        case 'd': {
            if (has_long) {
                long value = va_arg(args, long);
                if (value < 0) {
                    serial_putc('-');
                    print_unsigned((unsigned long)(0l - value), 10);
                } else {
                    print_unsigned((unsigned long)value, 10);
                }
            } else {
                int value = va_arg(args, int);
                if (value < 0) {
                    serial_putc('-');
                    print_unsigned((unsigned long)(0u - (unsigned)value), 10);
                } else {
                    print_unsigned((unsigned long)value, 10);
                }
            }
            i += spec_offset;
            break;
        }
        case 'u':
            print_unsigned(has_long ? va_arg(args, unsigned long)
                                     : va_arg(args, unsigned int), 10);
            i += spec_offset;
            break;
        case 'x':
            print_unsigned(has_long ? va_arg(args, unsigned long)
                                     : va_arg(args, unsigned int), 16);
            i += spec_offset;
            break;
        case 'p':
            print_pointer(va_arg(args, const void *));
            i += spec_offset;
            break;
        case 'c':
            serial_putc((char)va_arg(args, int));
            i += spec_offset;
            break;
        case 's': {
            const char *s = va_arg(args, const char *);
            serial_print(s ? s : "(null)");
            i += spec_offset;
            break;
        }
        case '%':
            serial_putc('%');
            i += spec_offset;
            break;
        default:
            serial_putc('%');
            break;
        }
    }
}

void printf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    acquireLog();
    vprintf_impl(fmt, args);
    releaseLog();
    va_end(args);
}

void logInfo(const char *fmt, ...) {
#if !VERBOSE_LOG
    (void)fmt;
    return;
#else
    va_list args;
    va_start(args, fmt);
    acquireLog();
    serial_print(COLOR_GREEN "[INFO] " COLOR_RESET);
    vprintf_impl(fmt, args);
    releaseLog();
    va_end(args);
#endif
}

void logSection(const char *fmt, ...) {
#if !VERBOSE_LOG
    (void)fmt;
    return;
#else
    va_list args;
    va_start(args, fmt);
    acquireLog();
    serial_print("\n" COLOR_CYAN COLOR_BOLD "== ");
    vprintf_impl(fmt, args);
    serial_print(" ==" COLOR_RESET "\n");
    releaseLog();
    va_end(args);
#endif
}

void logError(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    acquireLog();
    serial_print(COLOR_RED "[ERROR] " COLOR_RESET);
    vprintf_impl(fmt, args);
    releaseLog();
    va_end(args);
}
