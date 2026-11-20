/* serial_disk_loader.c
 * Host-side utility to send disk images to VMBR guest over serial port
 *
 * Communicates with the VMBR hypervisor running on the guest VM
 * to transfer Windows (or other OS) disk images over serial connection.
 *
 * Protocol:
 *   1. Guest sends PROTO_SYNC (0xAA) to signal readiness for a sector
 *   2. Host responds with PROTO_ACK (0x55) or PROTO_NACK (0xEE)
 *   3. If ACK, host sends 512-byte sector
 *   4. Guest acknowledges receipt with PROTO_ACK (0x55)
 *   5. Repeat until host sends PROTO_END (0xFF)
 *
 * Compile: gcc -o serial_disk_loader serial_disk_loader.c
 * Usage: ./serial_disk_loader /dev/ttyS0 /path/to/windows.img
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>
#endif

/* Protocol control bytes */
#define PROTO_SYNC  0xAA  /* Guest ready for sector */
#define PROTO_END   0xFF  /* Host signals end of transfer */
#define PROTO_ACK   0x55  /* Acknowledgment */
#define PROTO_NACK  0xEE  /* Negative acknowledgment */

#define SECTOR_SIZE 512
#define BAUD_RATE   38400
#define TIMEOUT_MS  2000

/* Global serial port descriptor */
static int serial_fd = -1;

/* Signal handler for cleanup */
void cleanup(int sig) {
    if (serial_fd >= 0) {
#ifndef _WIN32
        tcsetattr(serial_fd, TCSAFLUSH, NULL);
#endif
        close(serial_fd);
    }
    if (sig == SIGINT) {
        printf("\n[!] Interrupted by user\n");
        exit(1);
    }
}

#ifdef _WIN32
/* Windows serial port handling */
HANDLE open_serial_port_windows(const char *port_name) {
    HANDLE port = CreateFileA(
        port_name,
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );

    if (port == INVALID_HANDLE_VALUE) {
        printf("[!] Failed to open port %s\n", port_name);
        return INVALID_HANDLE_VALUE;
    }

    /* Configure port */
    DCB dcb = {0};
    dcb.DCBlength = sizeof(DCB);

    if (!GetCommState(port, &dcb)) {
        printf("[!] Failed to get port state\n");
        CloseHandle(port);
        return INVALID_HANDLE_VALUE;
    }

    dcb.BaudRate = BAUD_RATE;
    dcb.ByteSize = 8;
    dcb.StopBits = ONESTOPBIT;
    dcb.Parity = NOPARITY;

    if (!SetCommState(port, &dcb)) {
        printf("[!] Failed to set port state\n");
        CloseHandle(port);
        return INVALID_HANDLE_VALUE;
    }

    /* Set timeouts */
    COMMTIMEOUTS timeouts = {0};
    timeouts.ReadIntervalTimeout = TIMEOUT_MS;
    timeouts.ReadTotalTimeoutConstant = TIMEOUT_MS;
    timeouts.ReadTotalTimeoutMultiplier = 0;
    timeouts.WriteTotalTimeoutConstant = TIMEOUT_MS;
    timeouts.WriteTotalTimeoutMultiplier = 0;

    if (!SetCommTimeouts(port, &timeouts)) {
        printf("[!] Failed to set timeouts\n");
        CloseHandle(port);
        return INVALID_HANDLE_VALUE;
    }

    return port;
}

int serial_read_byte_windows(HANDLE port) {
    unsigned char byte;
    DWORD bytes_read;

    if (!ReadFile(port, &byte, 1, &bytes_read, NULL)) {
        return -1;
    }

    return (bytes_read == 1) ? (int)byte : -1;
}

int serial_write_bytes_windows(HANDLE port, const unsigned char *data, int len) {
    DWORD bytes_written;

    if (!WriteFile(port, data, len, &bytes_written, NULL)) {
        return -1;
    }

    return (int)bytes_written;
}
#else
/* Unix/Linux serial port handling */
int open_serial_port_unix(const char *port_name) {
    int fd = open(port_name, O_RDWR | O_NOCTTY | O_NDELAY);

    if (fd < 0) {
        perror("open");
        return -1;
    }

    /* Get current terminal settings */
    struct termios tio;
    if (tcgetattr(fd, &tio) != 0) {
        perror("tcgetattr");
        close(fd);
        return -1;
    }

    /* Set baud rate and other options */
    cfsetispeed(&tio, B38400);
    cfsetospeed(&tio, B38400);

    tio.c_cflag &= ~CSIZE;
    tio.c_cflag |= CS8;
    tio.c_cflag &= ~PARENB;
    tio.c_cflag &= ~CSTOPB;
    tio.c_cflag |= CREAD | CLOCAL;
    tio.c_iflag &= ~(IXON | IXOFF | IXANY);
    tio.c_oflag &= ~OPOST;
    tio.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);

    tio.c_cc[VTIME] = 20;  /* 2 second timeout */
    tio.c_cc[VMIN] = 0;

    if (tcsetattr(fd, TCSAFLUSH, &tio) != 0) {
        perror("tcsetattr");
        close(fd);
        return -1;
    }

    return fd;
}

int serial_read_byte_unix(int fd) {
    unsigned char byte;
    ssize_t n = read(fd, &byte, 1);
    return (n == 1) ? (int)byte : -1;
}

int serial_write_bytes_unix(int fd, const unsigned char *data, int len) {
    ssize_t n = write(fd, data, len);
    return (n > 0) ? (int)n : -1;
}
#endif

/* Load disk image from file and send over serial */
int load_disk_image(const char *serial_port, const char *image_path, int verbose) {
    FILE *f = NULL;
    unsigned char sector_buffer[SECTOR_SIZE];
    uint64_t file_size = 0;
    uint64_t file_sectors = 0;
    uint64_t sectors_sent = 0;
    int result = 0;

#ifdef _WIN32
    HANDLE port = open_serial_port_windows(serial_port);
    if (port == INVALID_HANDLE_VALUE) {
        return 0;
    }
#else
    int port = open_serial_port_unix(serial_port);
    if (port < 0) {
        return 0;
    }
#endif

    /* Get file size */
    struct stat st;
    if (stat(image_path, &st) != 0) {
        printf("[!] Cannot stat file: %s\n", image_path);
        goto cleanup;
    }

    file_size = st.st_size;
    file_sectors = (file_size + SECTOR_SIZE - 1) / SECTOR_SIZE;

    if (verbose) {
        printf("[*] Loading disk image: %s\n", image_path);
        printf("[*] File size: %llu bytes (%llu sectors)\n",
               (unsigned long long)file_size, (unsigned long long)file_sectors);
        printf("[*] Serial port: %s @ %d baud\n", serial_port, BAUD_RATE);
        printf("[*] Waiting for guest to signal readiness...\n");
    }

    /* Open image file */
    f = fopen(image_path, "rb");
    if (!f) {
        printf("[!] Cannot open file: %s\n", image_path);
        goto cleanup;
    }

    /* Main transfer loop */
    while (1) {
        int sync_byte;
        int ack_byte;
        size_t bytes_read;
        unsigned char response;

        /* Wait for guest PROTO_SYNC */
#ifdef _WIN32
        sync_byte = serial_read_byte_windows(port);
#else
        sync_byte = serial_read_byte_unix(port);
#endif

        if (sync_byte < 0) {
            if (sectors_sent == 0) {
                printf("[!] Timeout waiting for PROTO_SYNC at sector %llu\n",
                       (unsigned long long)sectors_sent);
            }
            break;
        }

        if (sync_byte != PROTO_SYNC) {
            if (verbose) {
                printf("[!] Unexpected byte: 0x%02x (expected 0x%02x)\n",
                       sync_byte, PROTO_SYNC);
            }
            continue;
        }

        /* Read one sector from disk image */
        memset(sector_buffer, 0, SECTOR_SIZE);
        bytes_read = fread(sector_buffer, 1, SECTOR_SIZE, f);

        if (bytes_read == 0) {
            /* End of file - send PROTO_END */
            response = PROTO_END;
#ifdef _WIN32
            serial_write_bytes_windows(port, &response, 1);
#else
            serial_write_bytes_unix(port, &response, 1);
#endif
            if (verbose) {
                printf("[+] End of file reached. Sent %llu sectors.\n",
                       (unsigned long long)sectors_sent);
            }
            result = 1;
            break;
        }

        /* Send PROTO_ACK and sector data */
        response = PROTO_ACK;
#ifdef _WIN32
        serial_write_bytes_windows(port, &response, 1);
        serial_write_bytes_windows(port, sector_buffer, SECTOR_SIZE);
#else
        serial_write_bytes_unix(port, &response, 1);
        serial_write_bytes_unix(port, sector_buffer, SECTOR_SIZE);
#endif

        /* Wait for guest acknowledgment */
#ifdef _WIN32
        ack_byte = serial_read_byte_windows(port);
#else
        ack_byte = serial_read_byte_unix(port);
#endif

        if (ack_byte == PROTO_ACK) {
            sectors_sent++;

            if (verbose && (sectors_sent % 32 == 0)) {
                double progress_pct = (double)sectors_sent / (double)file_sectors * 100.0;
                printf("[+] Progress: %llu/%llu sectors (%.1f%%)\n",
                       (unsigned long long)sectors_sent,
                       (unsigned long long)file_sectors,
                       progress_pct);
            }
        } else {
            printf("[!] No acknowledgment from guest at sector %llu\n",
                   (unsigned long long)sectors_sent);
        }
    }

    if (verbose) {
        printf("[+] Transfer complete!\n");
        printf("[+] Total: %llu sectors, %llu bytes\n",
               (unsigned long long)sectors_sent,
               (unsigned long long)(sectors_sent * SECTOR_SIZE));
    } else {
        printf("[+] Sent %llu sectors\n", (unsigned long long)sectors_sent);
    }

cleanup:
    if (f) fclose(f);
#ifdef _WIN32
    if (port != INVALID_HANDLE_VALUE) CloseHandle(port);
#else
    if (port >= 0) close(port);
#endif

    return result;
}

void print_usage(const char *prog_name) {
    printf("Usage: %s [options] <serial_port> <image_file>\n", prog_name);
    printf("\nOptions:\n");
    printf("  -v, --verbose      Verbose output\n");
    printf("  -h, --help         Show this help message\n");
    printf("\nExamples:\n");
    printf("  %s /dev/ttyS0 windows.img\n", prog_name);
    printf("  %s /dev/ttyUSB0 ubuntu.iso\n", prog_name);
    printf("  %s COM3 disk.img  (Windows)\n", prog_name);
}

int main(int argc, char *argv[]) {
    const char *serial_port = NULL;
    const char *image_path = NULL;
    int verbose = 0;
    int i;

    /* Parse arguments */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            verbose = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (argv[i][0] != '-') {
            if (!serial_port) {
                serial_port = argv[i];
            } else if (!image_path) {
                image_path = argv[i];
            }
        }
    }

    if (!serial_port || !image_path) {
        print_usage(argv[0]);
        return 1;
    }

    /* Setup signal handler for cleanup */
    signal(SIGINT, cleanup);

    /* Load disk image */
    int success = load_disk_image(serial_port, image_path, verbose);

    return success ? 0 : 1;
}
