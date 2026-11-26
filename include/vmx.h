/* vmx.h - see vmx.c */
#ifndef VMX_H
#define VMX_H

/* Checks VMX support, sets up CR4.VMXE and IA32_FEATURE_CONTROL,
 * allocates and enters the VMXON region, then allocates a VMCS region
 * and makes it current (VMCLEAR + VMPTRLD). Builds the EPT (ept.c),
 * fills in the controls and the guest/host state areas, copies the
 * guest in and VMLAUNCHes it. Logs each step; if anything fails, logs
 * the error and returns without going further.
 *
 * On success this does not return in the usual sense: the guest runs,
 * and every VM exit from it lands in vmexit_entry.asm's stub instead (see
 * vmexit.h), which resumes the guest once the exit is handled.
 */
void InitializeVmm(void);

/* AP bring-up into VMX non-root. Called once per AP from ap_main after
 * long-mode entry. Waits for the BSP to publish the shared EPT pointer
 * (set by InitializeVmm after its EPT is built), then per this CPU:
 *   - fix up CR0/CR4 and enable VMX
 *   - allocate and enter the VMXON region
 *   - allocate the VMCS region, VMCLEAR + VMPTRLD it
 *   - fill in the same host/control/guest layout the BSP uses, but
 *     with the guest initial RIP/CS pointing at the AP guest stub so
 *     this AP's non-root execution starts in a tiny halt loop until
 *     the guest OS wakes it (handled by INIT/SIPI VM-exits later).
 *   - VMLAUNCH.
 * Does not return on success.
 */
void apBringUp(void);

/* SIPI handler support (vmexit.c calls this). Rewrites the CURRENT
 * VMCS's guest CS selector/base/limit/AR so the guest resumes in real
 * mode at CS:IP = (vector:0000), i.e. physical (vector << 12). The SDM
 * says a SIPI with vector VV starts the AP at linear 0x000VV000H -
 * exactly what writeRealModeSegment produces when its selector input
 * is (vector << 8). Also resets GUEST_RIP to 0 and clears any STI/MOV-
 * SS interruptibility blocking. Does NOT change activity state - the
 * caller sets it to ACTIVE after this returns, so a bad VMCS edit
 * stays detectable before the guest resumes. */
void setGuestSipiStartPoint(uint8_t vector);

#endif
