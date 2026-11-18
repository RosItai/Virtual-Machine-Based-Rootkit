/* kernel.c
 * A minimal kernel that proves the boot chain worked by printing
 * centered ASCII art over the serial port.
 */
#include <stdint.h>
#include "serial.h"
#include "debug.h"
#include "memory_map.h"
#include "vmx.h"
#include "host_paging.h"
#include "colors.h"

/* --- Serial port (COM1) I/O --- */
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

#define COM1 0x3F8

void serial_init(void) {
    outb(COM1 + 1, 0x00); /* disable interrupts */
    outb(COM1 + 3, 0x80); /* enable DLAB to set baud rate divisor */
    outb(COM1 + 0, 0x03); /* divisor low byte -> 38400 baud */
    outb(COM1 + 1, 0x00); /* divisor high byte */
    outb(COM1 + 3, 0x03); /* 8 bits, no parity, one stop bit */
    outb(COM1 + 2, 0xC7); /* enable + clear FIFO */
    outb(COM1 + 4, 0x0B); /* mark data terminal ready */
}

static int serial_transmit_empty(void) { return inb(COM1 + 5) & 0x20; }
static int serial_data_ready(void)      { return inb(COM1 + 5) & 0x01; }

void serial_putc(char c) {
    while (!serial_transmit_empty());
    outb(COM1, c);
}
void serial_print(const char *s) { while (*s) serial_putc(*s++); }

static int str_len(const char *s) { int n = 0; while (s[n]) n++; return n; }

static void serial_print_uint(unsigned int n) {
    if (n >= 10) serial_print_uint(n / 10);
    serial_putc('0' + (n % 10));
}

static void cursor_goto(int row, int col) {
    serial_print("\033[");
    serial_print_uint((unsigned int)row);
    serial_putc(';');
    serial_print_uint((unsigned int)col);
    serial_putc('H');
}

/* --- Ask the terminal for its real size ---
 * "\033[18t" is a long-standing xterm extension: a well-behaved
 * terminal answers with "\033[8;ROWS;COLSt" over the same channel.
 * We read that reply back over the serial line's RX side (which is
 * connected to the same terminal since QEMU's -nographic wires the
 * serial port to your stdin/stdout). If nothing valid comes back
 * within the timeout, we fall back to a safe default. */
#define RESP_MAX 32
#define FALLBACK_WIDTH 80
#define TIMEOUT_LOOPS 20000000UL

static int parse_uint(const char *s, int *i) {
    int val = 0;
    while (s[*i] >= '0' && s[*i] <= '9') {
        val = val * 10 + (s[*i] - '0');
        (*i)++;
    }
    return val;
}

static int query_terminal_width(void) {
    char buf[RESP_MAX];
    int len = 0;
    unsigned long idle = 0;

    serial_print("\033[18t");

    while (len < RESP_MAX - 1) {
        if (serial_data_ready()) {
            char c = (char)inb(COM1);
            buf[len++] = c;
            idle = 0;
            if (c == 't') break;
        } else {
            idle++;
            if (idle > TIMEOUT_LOOPS) break;
        }
    }
    buf[len] = 0;

    /* Expect: ESC [ 8 ; <rows> ; <cols> t  -- find '[' then walk it */
    int i = 0;
    while (i < len && buf[i] != '[') i++;
    if (i >= len) return -1;
    i++;
    if (buf[i] != '8') return -1;
    i++;
    if (buf[i] != ';') return -1;
    i++;
    parse_uint(buf, &i); /* rows - not needed here */
    if (buf[i] != ';') return -1;
    i++;
    int cols = parse_uint(buf, &i);
    if (cols <= 0) return -1;
    return cols;
}

#define TOP_ROW 1

static const char *art[] = {
    " __     ____  __ ____  ____  ",
    " \\ \\   / /  \\/  | __ )|  _ \\ ",
    "  \\ \\ / /| |\\/| |  _ \\| |_) |",
    "   \\ V / | |  | | |_) |  _ < ",
    "    \\_/  |_|  |_|____/|_| \\_\\",
    0
};

static const char *author = "by: Itai Rosenperl";

static int leading_spaces(const char *s) {
    int n = 0;
    while (s[n] == ' ') n++;
    return n;
}

/* All art lines share the same length, so they all land on the same
 * starting column when centered - but they don't all have the same
 * number of leading spaces inside the string (the top of the V only
 * needs 1, lower rows need more to taper), so the visible ink doesn't
 * all start at that column. Find the leftmost actual ink column across
 * every row and align the author line to THAT, not to base_col itself. */
static void print_art_and_author(int term_width) {
    int num_lines = 0;
    while (art[num_lines]) num_lines++;

    int art_len = str_len(art[0]);
    int base_col = (term_width - art_len) / 2 + 1;
    if (base_col < 1) base_col = 1;

    int min_lead = leading_spaces(art[0]);
    for (int i = 1; i < num_lines; i++) {
        int lead = leading_spaces(art[i]);
        if (lead < min_lead) min_lead = lead;
    }
    int ink_col = base_col + min_lead + 4; /* nudged 4 cols right per visual feedback */

    serial_print(COLOR_BLUE);
    for (int i = 0; i < num_lines; i++) {
        cursor_goto(TOP_ROW + i, base_col);
        serial_print(art[i]);
    }

    /* Author line goes on the row right after the art - no gap - at
     * the leftmost ink column, not the raw string's start column. */
    cursor_goto(TOP_ROW + num_lines, ink_col);
    serial_print(author);
    serial_print(COLOR_RESET);
}

void kernel_main(void) {
    serial_init();
    serial_print("\033[2J");
    
    int width = query_terminal_width();
    if (width <= 0) width = FALLBACK_WIDTH;

    print_art_and_author(width);
    serial_print("\r\n");

    logSection("Long Mode");
    logInfo("64-bit code executing\n");

    /* long_mode.asm calls bios_load_memory_map (the BIOS E820 call)
     * before calling kernel_main - the map is already sitting at
     * MEMORY_MAP_TABLE_ADDR by the time we get here, so this is a
     * plain pointer read, no further BIOS round trip needed. */
    MemoryMapTable *memoryMap = (MemoryMapTable *)MEMORY_MAP_TABLE_ADDR;
    logSection("BIOS E820 Memory Map");
    logInfo("%lu entries loaded\n", (unsigned long)memoryMap->numberOfEntries);

    /* Before InitializeVmm: HOST_CR3 is taken from the live CR3, so the
     * VMCS picks up the new tables automatically. */
    logSection("Host Paging");
    if (!initializeHostPaging()) {
        return;
    }

    InitializeVmm();

    /* Returns here instead of halting - long_mode_entry (long_mode.asm)
     * halts the CPU right after this call. */
}
