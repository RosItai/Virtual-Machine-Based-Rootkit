# Bare-Metal Hypervisor Rootkit

## Project layout

```
.
├── Makefile
├── include/           <- put gdt.h, paging.h, debug.h etc. here later
└── src/
    ├── boot.asm       <- Multiboot2 header + entry point (_start)
    ├── kernel.c       <- kernel_main and everything else
    ├── linker_script.ld
    └── boot/
        └── grub.cfg
```

New `.c`/`.asm` files dropped anywhere under `src/` are picked up
automatically by the Makefile (via `find`) - no Makefile edits needed
as the project grows (e.g. adding `src/gdt.c`, `src/paging.c`,
`src/debug.c`).

## One-time setup (Ubuntu)

```bash
sudo apt install nasm gcc-multilib grub-pc-bin grub-common xorriso mtools qemu-system-x86
```

## Building and running

```bash
make          # builds build/vmbr.iso (only rebuilds what changed)
make run      # builds (if needed) and boots in QEMU, output in this terminal
make debug    # same, but pauses at boot with a GDB stub on :1234 for GEF/gdb
make clean    # removes the build/ directory entirely
```

All generated files (`.o`, `vmbr.bin`, `vmbr.iso`, the ISO staging
tree) live under `build/`, which `make clean` wipes completely without
touching anything under `src/`.

## Current status

This is currently a **32-bit** kernel booted via GRUB's Multiboot2
protocol, running in QEMU's `-nographic` mode (serial console, no
graphical window). Output is centered ASCII art printed over the
serial port using ANSI cursor positioning.

## Planned next steps

Paging -> GDT -> debugging utilities -> switch to Long Mode (64-bit).
When that switch happens, three things in the Makefile need to change
together (they're marked with `***` comments inline):
- `CFLAGS`: `-m32` -> `-m64`
- `ASM_ASSEMBLER_FLAGS`: `-f elf32` -> `-f elf64`
- `LINKER_FLAGS`: `-m elf_i386` -> `-m elf_x86_64`, and the `--oformat`
  accordingly

## The boot sequence, end to end

1. `qemu-system-i386 -cdrom build/vmbr.iso -nographic` - QEMU's virtual
   BIOS treats the ISO like a real bootable CD, with no graphical
   window (serial console redirected to this terminal instead).
2. The ISO's El Torito boot record points at GRUB's own boot images
   (embedded automatically by `grub-mkrescue`).
3. GRUB reads `src/boot/grub.cfg` (copied into the ISO at build time)
   and boots the single menu entry immediately.
4. GRUB loads `build/vmbr.bin`, validates its Multiboot2 header,
   switches to 32-bit protected mode, and jumps to `_start`.
5. `_start` (in `boot.asm`) sets up a stack and calls `kernel_main`.
6. `kernel_main` prints the centered ASCII art over the serial port,
   then halts in a loop.

## If it doesn't boot

- **"Multiboot2 header not found" or GRUB drops to a rescue shell** -
  check the header is within the first 32KB of `vmbr.bin` and that
  `linker_script.ld` places `.multiboot_header` first.
- **Link errors about missing 32-bit libraries** - confirm
  `gcc-multilib` is installed (`dpkg -l | grep multilib`).
- **`grub-mkrescue` fails with an xorriso/mtools error** - those two
  tools are separate from the grub packages; install them explicitly.
- **"missing separator" from make** - a recipe line under a target
  got a tab converted to spaces. Recipe lines must start with a
  literal Tab character, not spaces.
