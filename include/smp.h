/* smp.h
 * SMP (Symmetric Multi-Processing) bring-up: wakes the APs that
 * firmware parked in wait-for-SIPI and brings them into VMBR's code.
 * See smp.c.
 */
#ifndef SMP_H
#define SMP_H

#include <stdint.h>

/* Copy the AP trampoline into low memory and send INIT-SIPI-SIPI to
 * every AP the ACPI MADT lists. Returns how many APs actually arrived
 * at the trampoline (read from a shared counter the trampoline bumps). */
int wakeAps(int totalCpuCount);

#endif
