/* ap.c
 * AP (Application Processor) bring-up. Each AP enters here after
 * boot.asm has given it a unique stack and long_mode.asm has put it in
 * 64-bit mode. Phase-A stub for now: just prove each AP reached here,
 * then halt. Later phases will VMXON+VMLAUNCH a per-AP VMCS so the AP
 * runs the guest alongside the BSP's guest.
 */
#include <stdint.h>
#include "ap.h"
#include "cpu.h"
#include "debug.h"

/* logInfo isn't reentrant - interleaved calls from multiple cores would
 * corrupt the serial output. A tiny spinlock around each AP's one-off
 * "I made it" line is enough while they're still trickling in one by
 * one; once there's more AP code than a single log line, revisit. */
static volatile uint32_t apLogLock = 0;

static void acquireLog(void) {
    while (__sync_lock_test_and_set(&apLogLock, 1)) {
        __asm__ volatile("pause");
    }
}

static void releaseLog(void) {
    __sync_lock_release(&apLogLock);
}

/* APIC ID is CPUID.1:EBX[31:24] - works in long mode on every x86-64
 * CPU; VMBR hides nothing about this leaf from itself. */
static uint32_t localApicId(void) {
    uint32_t eax, ebx, ecx, edx;
    cpuid(1, &eax, &ebx, &ecx, &edx);
    return ebx >> 24;
}

void ap_main(void) {
    uint32_t id = localApicId();

    acquireLog();
    logInfo("AP %u reached long mode\n", id);
    releaseLog();

    /* Halt forever for now - the next phase replaces this with a VMXON
     * + VMLAUNCH into a per-AP guest. */
    for (;;) {
        __asm__ volatile("cli\n\thlt");
    }
}
