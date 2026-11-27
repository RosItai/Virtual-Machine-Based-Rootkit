# ----------------------------------------------------------------------
# C Compiler
# ----------------------------------------------------------------------
CC = gcc
# EXTRA_CFLAGS is how the interactive run-win10 target threads -D
# toggles like DEBUG_INT13=1 and DEBUG_PORT_IO=1 into the compile without
# changing the committed defaults. Normal builds leave it empty.
EXTRA_CFLAGS ?=
CFLAGS = -c -nostdlib -fno-builtin -fno-stack-protector -m64 -mno-red-zone -mcmodel=large -ffreestanding -fno-pic -g -I./include $(EXTRA_CFLAGS)
# -c:                Compile source files into object files without linking.
# -nostdlib:         Do not link against standard libraries.
# -fno-builtin:      Do not treat standard functions as built-in, preventing compiler optimizations for them.
# -fno-stack-protector: Disable stack protection features.
# -m64:              64-bit build - kernel_main and everything it calls
#                    (allocateMemory, etc.) now run in long mode, after
#                    the switch, so they need real 64-bit code.
# -mno-red-zone:     The x86-64 SysV ABI's 128-byte "red zone" below RSP
#                    assumes nothing else (like an interrupt handler)
#                    will ever reuse that stack space uninvited - true
#                    for hosted userspace, not for kernel code, which
#                    could be interrupted at any point.
# -mcmodel=large:    Don't assume any code/data symbol fits in a 2GB
#                    range relative to the instruction pointer or a
#                    fixed base - avoids relying on addressing-range
#                    assumptions PIC/PIE cares about, for a kernel
#                    linked at a fixed low address (1MB) with no
#                    dynamic loader.
# -ffreestanding:    No hosted environment - no libc, no assumptions about main().
# -fno-pic:          No position-independent code - we load at a fixed address.
# -g:                Emit debug symbols, for use with the "debug" target below.
# -I./include:       Add ./include to the list of directories searched for header files.
# (No -nostdinc: stdint.h/stddef.h are compiler-provided freestanding
#  headers, not libc, so they're safe and needed even bare-metal.)

# ----------------------------------------------------------------------
# ASM Assembler
# ----------------------------------------------------------------------
ASM_ASSEMBLER = nasm
ASM_ASSEMBLER_FLAGS = -f elf64 -w-all -g -F dwarf
# -f elf64:  Generates output in the 64-bit ELF format. Early boot code
#            (boot.asm, most of paging.asm/gdt.asm/long_mode.asm) is
#            still genuine 16/32-bit machine code via explicit "bits"
#            directives - that's independent of the *object file's own*
#            ELF class, which only affects how the linker reads
#            symbols/relocations. This mixing is the same pattern
#            already used successfully within bios_call.asm's single
#            file, just now applied kernel-wide.
# -w-all:    Suppresses all warning messages during assembly.
# -g:        Enables generation of debugging information for use with debuggers.
# -F dwarf:  Specifies the use of the DWARF format for the generated debugging information.

# ----------------------------------------------------------------------
# Linker
# ----------------------------------------------------------------------
LINKER = ld
LINKER_FLAGS = -m elf_x86_64 -T src/linker_script.ld --oformat elf64-x86-64 -n
# -m elf_x86_64:      64-bit x86 ELF target.
# -T src/linker_script.ld: Uses the specified linker script to control the linking process and output file layout.
# --oformat elf64-x86-64: Produces a 64-bit ELF file for x86-64. GRUB2's
#                    multiboot2 loader supports a 64-bit kernel ELF just
#                    fine - the Multiboot2 header's own "architecture"
#                    tag (boot.asm) separately says the CPU must be in
#                    32-bit protected mode *at entry*, regardless of the
#                    ELF's own class.
# -n:                 Disables relocations, meaning addresses are not adjusted during linking.

# ----------------------------------------------------------------------
GRUB_CFG := src/boot/grub.cfg
C_FILES := $(shell find src/ -name '*.c')
# macros.asm and memory_map.asm are %include-only (pulled into other
# .asm files at assemble time) - neither has a build/%.o rule of its
# own and shouldn't be compiled standalone.
ASSEMBLY_FILES := $(filter-out src/boot/macros.asm src/boot/memory_map.asm, $(shell find src/ -name '*.asm'))

# Object Files
# Every .c/.asm file found under src/ automatically gets its own .o in
# build/ - add gdt.c, paging.c, debug.c under src/ later and they show
# up here with no Makefile changes needed.
C_OBJECT_FILES := $(addprefix build/, $(notdir $(C_FILES:.c=.o)))
ASM_OBJECT_FILES := $(addprefix build/, $(notdir $(ASSEMBLY_FILES:.asm=.o)))
OBJECT_FILES := $(C_OBJECT_FILES) $(ASM_OBJECT_FILES)

build: build/vmbr.iso

# -enable-kvm -cpu host: needed from the VMX iteration onward - plain
# TCG (software-emulated) QEMU generally can't execute real VMX
# instructions (VMXON/etc. simply fail), since our kernel is itself
# trying to run as a hypervisor *inside* this QEMU guest (nested
# virtualization). This needs /dev/kvm to exist and be usable, which
# under WSL2 in turn needs Hyper-V's nested-virtualization support to
# be exposed to the WSL2 VM - reportedly unreliable on Windows 10,
# working on Windows 11. If VMXON fails with VMX reported as
# unsupported, check that first (`ls -la /dev/kvm`) before suspecting
# the code.
# -machine smm=off: QEMU's SMM emulation (used by SeaBIOS) has known
# interactions that can corrupt/confuse nested-VMX guest state -
# disabling it entirely sidesteps that class of bug.
# DISK_IMG is attached as the first hard disk (BIOS drive 0x80) so the
# int 0x13 disk driver has something to read. Swap in a real Windows
# image with:  make run DISK_IMG=/path/to/win10.qcow2 DISK_FMT=qcow2 MEM=4G
# DISK_FMT must match the image (raw for test_disk.img, qcow2 for the
# Windows disk tools/install_windows.sh creates). MEM is the guest RAM -
# bump it for Windows.
DISK_IMG ?= test_disk.img
DISK_FMT ?= raw
MEM      ?= 2G
SMP      ?= 1

# Shortcut: boot the real Windows 10 install under VMBR with SMP enabled.
# Lives beside `run` so the full qemu command stays in one place - this
# just forwards to `run` with the Win10 image/format/RAM baked in and SMP
# defaulting to 4 (override on the command line if you want a different
# count). Keeps the long `DISK_IMG=... DISK_FMT=qcow2 MEM=8G` form out of
# day-to-day use.
WIN10_IMG ?= $(abspath Win10/win10.qcow2)

# Four explicit targets, picked by how chatty you want the serial log:
#   run-win10        /  run-win10-gui      - silent: no INT 13h, no port I/O
#   run-win10-log    /  run-win10-gui-log  - verbose: both logs on
# SIPI/INIT lines, guest-hook installation lines, EPT hiding, VMCS init
# and every error/fatal message print in all four - DEBUG_INT13 /
# DEBUG_PORT_IO control only the two bulk categories that eclipse the
# rest of the log at Windows boot speed (and that historically burned
# through the guest's 100ms DPC watchdog). The verbose targets recompile
# just hooks.o and vmexit.o with -B so toggling takes effect without a
# full `make clean`.
WIN10_SMP := $(if $(filter-out 1,$(SMP)),$(SMP),4)
define RUN_WIN10
	$(MAKE) -B build/debug.o build/hooks.o build/vmexit.o EXTRA_CFLAGS="$(2)" >/dev/null
	$(MAKE) $(1) EXTRA_CFLAGS="$(2)" DISK_IMG=$(WIN10_IMG) DISK_FMT=qcow2 MEM=8G SMP=$(WIN10_SMP)
endef

# -log adds the per-port, per-INT-13h and the whole progress stream
# (logInfo / logSection). Without -log, logInfo/logSection are compile-
# time no-ops - the serial console shows only the big ASCII banner plus
# any genuine error line.
WIN10_LOG_FLAGS := -DVERBOSE_LOG=1 -DDEBUG_INT13=1 -DDEBUG_PORT_IO=1

run-win10:
	$(call RUN_WIN10,run,)

run-win10-gui:
	$(call RUN_WIN10,run-gui,)

run-win10-log:
	$(call RUN_WIN10,run,$(WIN10_LOG_FLAGS))

run-win10-gui-log:
	$(call RUN_WIN10,run-gui,$(WIN10_LOG_FLAGS))

# Boot the Windows installer ISO against the installed Windows qcow2, NO
# VMBR in the loop. Lets you reach WinRE "Repair your computer" ->
# Troubleshoot -> Advanced -> Command Prompt to clear the "boot keeps
# failing, force Automatic Repair" marker that our earlier BSOD/triple-
# fault runs left in the BCD:
#   bcdedit /set {default} recoveryenabled No
#   bcdedit /set {default} bootstatuspolicy IgnoreAllFailures
# Then close the window, and plain `make run-win10` will boot straight
# into Windows again instead of looping on Automatic Repair.
WIN10_INSTALL_ISO ?= $(abspath Win10/Windows.iso)
fix-win10-bcd:
	qemu-system-x86_64 -cdrom $(WIN10_INSTALL_ISO) -drive file=$(WIN10_IMG),format=qcow2,index=0,media=disk -boot d -m 4G -smp 2 -enable-kvm -cpu host -machine smm=off -usb -device usb-tablet -device usb-kbd -display gtk

run: build
	qemu-system-x86_64 -cdrom build/vmbr.iso -drive file=$(DISK_IMG),format=$(DISK_FMT),index=0,media=disk -boot d -m $(MEM) -smp $(SMP) -enable-kvm -cpu host -machine smm=off -nographic -no-reboot -d cpu_reset 2>&1 | tee /tmp/vmbr_last_run.log

# Like `run`, but with the VGA window open so the guest's screen is visible
# alongside VMBR's serial log. Useful when the guest halts silently - any
# "BOOTMGR is missing" / "Invalid disk" / BSOD text only lives on the video
# channel, which -nographic throws away. `-serial stdio` keeps VMBR's log
# flowing to the current terminal unbuffered (a `-serial file:X` first try
# buffered the file and lost every pre-crash byte); the tee below also
# saves a copy for later.
run-gui: build
	qemu-system-x86_64 -cdrom build/vmbr.iso -drive file=$(DISK_IMG),format=$(DISK_FMT),index=0,media=disk -boot d -m $(MEM) -smp $(SMP) -enable-kvm -cpu host -machine smm=off -serial stdio -no-reboot -usb -device usb-tablet -device usb-kbd -display gtk -d cpu_reset 2>&1 | tee /tmp/vmbr_last_run.log

# "debug" boots QEMU paused (-S) with a GDB stub open on port 1234 (-s),
# for use once you've got debug utilities and want to attach GDB/GEF:
#   (gdb) target remote localhost:1234
# -smp 1: boot.asm doesn't yet check BSP-vs-AP and park secondary cores,
# so with -smp 4 all 4 vCPUs ran this entire boot sequence concurrently,
# racing on the shared low-memory scratch the BIOS trampoline uses -
# confirmed via gdb (3 of 4 threads halted at the identical garbage
# address). Revisit once boot.asm parks APs deliberately.
debug: build
	qemu-system-x86_64 -cdrom build/vmbr.iso -drive file=$(DISK_IMG),format=$(DISK_FMT),index=0,media=disk -boot d -m $(MEM) -smp $(SMP) -enable-kvm -cpu host -machine smm=off -s -S -nographic -no-reboot

build/%.o: src/%.c
	@mkdir -p build
	$(CC) $(CFLAGS) $< -o $@

build/%.o: src/*/%.c
	@mkdir -p build
	$(CC) $(CFLAGS) $< -o $@

build/%.o: src/%.asm
	@mkdir -p build
	$(ASM_ASSEMBLER) $(ASM_ASSEMBLER_FLAGS) $< -o $@

build/vmbr.bin: $(OBJECT_FILES) src/linker_script.ld
	@mkdir -p build
	$(LINKER) $(LINKER_FLAGS) $(OBJECT_FILES) -o $@

build/vmbr.iso: build/vmbr.bin $(GRUB_CFG)
	@mkdir -p build/isofiles/boot/grub
	@cp $(GRUB_CFG) build/isofiles/boot/grub
	@cp $< build/isofiles/boot
	grub-mkrescue -o $@ build/isofiles

clean:
	rm -rf build

.PHONY: build run debug clean
# make build && make run-gui DISK_IMG=/mnt/d/CyberProject12/VMBR/Win10/win10.qcow2 DISK_FMT=qcow2 MEM=8G