/* linker.ld
 * Controls where each piece of the kernel ends up in memory.
 * Two things matter for Multiboot2 to work:
 *   1. The kernel loads at 1MB (0x100000) - the traditional safe spot,
 *      below memory holes and above the BIOS/boot area.
 *   2. The .multiboot_header section comes first in the file, so it's
 *      guaranteed to be within the first 32KB GRUB scans.
 */
ENTRY(_start)

SECTIONS
{
    . = 1M;

    /* The kernel image's own bounds, for the memory the VMM has to
     * protect from the guest: everything between these two symbols is
     * host code and data sitting in ordinary, guest-reachable RAM.
     * Declared here rather than guessed in C, because only the linker
     * knows where .bss actually ends. */
    __kernel_start = .;

    .text ALIGN(4K) :
    {
        *(.multiboot_header)
        *(.text)
    }

    .rodata ALIGN(4K) :
    {
        *(.rodata)
    }

    .data ALIGN(4K) :
    {
        *(.data)
    }

    .bss ALIGN(4K) :
    {
        *(COMMON)
        *(.bss)
    }

    __kernel_end = ALIGN(4K);
}
