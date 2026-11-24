/* smp.c
 * SMP bring-up: copies the real-mode AP trampoline into low memory,
 * then sends the Intel SDM 10.4.4.1 "INIT, SIPI, SIPI" sequence to
 * every AP the firmware reports. Each AP wakes up at the SIPI vector
 * (page number) * 0x1000, runs the trampoline, and reports back by
 * bumping the shared arrival counter.
 *
 * Phase-A scope: just prove we can wake every AP. Later phases turn the
 * trampoline into a full real-protected-long transition into ap_main.
 */
#include <stdint.h>
#include "smp.h"
#include "acpi.h"
#include "cpu.h"
#include "debug.h"
#include "string.h"

/* Symbols from ap_trampoline.asm: the blob's start/end labels (to
 * compute its byte length) and the fixed low-memory addresses the
 * trampoline writes to. */
extern uint8_t ap_trampoline_start[];
extern uint8_t ap_trampoline_end[];
extern uint32_t ap_trampoline_addr_val;    /* where the blob gets copied */
extern uint32_t ap_arrival_counter_val;    /* what APs lock-inc on arrival */

/* Local APIC - MMIO mode (xAPIC). Enough for wake-up IPIs; x2APIC would
 * use MSR 0x830 instead, but xAPIC is universal. The ICR is split in
 * two 32-bit registers 16 bytes apart: write high (destination) first,
 * then low (vector + delivery mode) which also *sends* the IPI. */
#define LAPIC_BASE          0xFEE00000ull
#define LAPIC_ICR_LOW       0x300u
#define LAPIC_ICR_HIGH      0x310u

#define ICR_DELIVERY_INIT   (5u << 8)
#define ICR_DELIVERY_SIPI   (6u << 8)
#define ICR_LEVEL_ASSERT    (1u << 14)
#define ICR_TRIGGER_EDGE    (0u << 15)
#define ICR_DEST_PHYSICAL   (0u << 11)
#define ICR_STATUS_PENDING  (1u << 12)

static volatile uint32_t *lapicReg(uint32_t offset) {
    return (volatile uint32_t *)(LAPIC_BASE + offset);
}

/* Wait until the IPI we just sent finishes dispatching: the CPU clears
 * the Delivery Status bit in ICR_LOW only once the IPI is on its way,
 * and the SDM is explicit that the next write must wait for this. */
static void waitForIpiIdle(void) {
    while (*lapicReg(LAPIC_ICR_LOW) & ICR_STATUS_PENDING) {
        __asm__ volatile("pause");
    }
}

/* Spin a known-wide-margin busy loop. The SDM wants 10ms between INIT
 * and SIPI and 200us between SIPIs, but we don't have calibrated
 * timers yet - a large plain loop is well over the 200us floor at
 * KVM speeds and safe under 10ms either way. */
static void spinDelay(uint64_t iterations) {
    for (volatile uint64_t i = 0; i < iterations; i++) {
        __asm__ volatile("pause");
    }
}

static void sendIpi(uint32_t apicId, uint32_t icrLowFlags) {
    *lapicReg(LAPIC_ICR_HIGH) = apicId << 24;
    *lapicReg(LAPIC_ICR_LOW)  = icrLowFlags;
    waitForIpiIdle();
}

static void sendInitSipiSipi(uint32_t apicId, uint8_t sipiVector) {
    /* Step 1: INIT. Vector is ignored for INIT delivery mode. */
    sendIpi(apicId, ICR_DELIVERY_INIT | ICR_LEVEL_ASSERT | ICR_TRIGGER_EDGE);
    spinDelay(1000000);

    /* Steps 2+3: two SIPIs, same vector. The SDM recommends sending
     * SIPI twice because the first may be lost if the AP hasn't quite
     * finished the INIT transition when it arrives. */
    sendIpi(apicId, ICR_DELIVERY_SIPI | ICR_LEVEL_ASSERT | sipiVector);
    spinDelay(100000);
    sendIpi(apicId, ICR_DELIVERY_SIPI | ICR_LEVEL_ASSERT | sipiVector);
    spinDelay(100000);
}

int wakeAps(int totalCpuCount) {
    /* 1. Copy the trampoline blob into its runtime slot. The blob is
     *    position-independent (see ap_trampoline.asm's BLOB_OFF/FLAT
     *    macros) so a straight memcpy is enough. */
    uint64_t blobSize = (uint64_t)(ap_trampoline_end - ap_trampoline_start);
    void *trampolineSlot = (void *)(uint64_t)ap_trampoline_addr_val;
    memcpy(trampolineSlot, ap_trampoline_start, blobSize);

    /* 2. Zero the arrival counter so we're measuring THIS boot's APs. */
    volatile uint32_t *counter = (volatile uint32_t *)(uint64_t)ap_arrival_counter_val;
    *counter = 0;

    /* 3. Figure out our own APIC ID (CPUID.1:EBX[31:24]) so we don't
     *    try to INIT ourselves - sending INIT to the BSP would reset us. */
    uint32_t eax, ebx, ecx, edx;
    cpuid(1, &eax, &ebx, &ecx, &edx);
    uint32_t bspApicId = ebx >> 24;

    /* 4. Walk APIC IDs 0..totalCpuCount-1 and INIT-SIPI each AP.
     *    QEMU assigns APIC IDs contiguously starting at 0, so this is
     *    a safe shortcut here; a real MADT walk would be more portable. */
    uint8_t sipiVector = (uint8_t)(ap_trampoline_addr_val >> 12);
    int apsTargeted = 0;
    for (uint32_t id = 0; id < (uint32_t)totalCpuCount; id++) {
        if (id == bspApicId) continue;
        sendInitSipiSipi(id, sipiVector);
        apsTargeted++;
    }

    /* 5. Give the APs time to reach the trampoline and bump the
     *    counter, then read it back. */
    spinDelay(5000000);
    int arrived = (int)*counter;

    logInfo("SMP: BSP APIC ID %u, sent INIT-SIPI (vector=0x%x) to %d AP(s), %d arrived\n",
            bspApicId, sipiVector, apsTargeted, arrived);
    return arrived;
}
