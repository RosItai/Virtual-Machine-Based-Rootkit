# Makefile for VMBR host-side tools

CC = gcc
CFLAGS = -Wall -Wextra -O2 -std=c99

# Detect Windows vs Unix/Linux
ifeq ($(OS),Windows_NT)
    EXE_SUFFIX = .exe
else
    EXE_SUFFIX =
endif

TARGETS = serial_disk_loader$(EXE_SUFFIX)

.PHONY: all clean

all: $(TARGETS)

serial_disk_loader$(EXE_SUFFIX): serial_disk_loader.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGETS) *.o

help:
	@echo "VMBR Host Tools"
	@echo ""
	@echo "Targets:"
	@echo "  all                - Build all tools"
	@echo "  serial_disk_loader - Build serial disk loader tool"
	@echo "  clean              - Remove built files"
	@echo ""
	@echo "Examples:"
	@echo "  make                # Build all tools"
	@echo "  make serial_disk_loader"
	@echo ""
	@echo "Usage:"
	@echo "  ./serial_disk_loader /dev/ttyS0 windows.img"
	@echo "  ./serial_disk_loader -v COM3 disk.img"
