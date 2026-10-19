/* vmx.h - see vmx.c */
#ifndef VMX_H
#define VMX_H

/* Checks VMX support, sets up CR4.VMXE and IA32_FEATURE_CONTROL,
 * allocates and enters the VMXON region, then allocates a VMCS region
 * and makes it current (VMCLEAR + VMPTRLD). Logs each step; if
 * anything fails, logs the error and returns without going further.
 *
 * Guest/host state area and VM-execution/exit/entry controls
 * (initializeVmcs in the reference this was ported from) are a
 * separate, later step - not implemented yet.
 */
void InitializeVmm(void);

#endif
