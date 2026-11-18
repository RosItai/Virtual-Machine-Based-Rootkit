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

#endif
