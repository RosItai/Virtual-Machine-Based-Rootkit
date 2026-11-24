/* ap.h
 * AP (Application Processor) bring-up entry point. Called from
 * long_mode.asm's split, once per AP, after boot.asm has given the AP
 * its own stack and the shared GDT/paging are in place. See ap.c.
 */
#ifndef AP_H
#define AP_H

/* Never returns. Each AP eventually halts in VMX non-root running a
 * tiny guest stub (same shape as the BSP's guest, just a different
 * initial VMCS). */
void ap_main(void);

#endif
