/* ap.c
 * AP (Application Processor) bring-up. Each AP enters here after
 * boot.asm has given it a unique stack and long_mode.asm has put it
 * in 64-bit mode. Each AP waits for the BSP to publish the shared
 * EPTP, then runs per-CPU VMXON + VMCS + VMLAUNCH to land in VMX
 * non-root alongside the BSP's guest.
 */
#include "ap.h"
#include "debug.h"
#include "vmx.h"

void ap_main(void) {
    /* Per-AP VMXON + VMCS + VMLAUNCH. apBringUp waits for the BSP to
     * publish the shared EPTP, then enters VMX non-root running the
     * AP guest stub (a tiny halt loop) until the guest OS wakes this
     * CPU with INIT-SIPI (handled by the INIT/SIPI exit handlers in a
     * later phase). Does not return on success; on failure it logs an
     * error and halts. logInfo/logError are serialised by debug.c's
     * own lock, so no AP-side lock is needed here. */
    apBringUp();

    logError("ap_main: apBringUp returned - halting\n");
    for (;;) {
        __asm__ volatile("cli\n\thlt");
    }
}
