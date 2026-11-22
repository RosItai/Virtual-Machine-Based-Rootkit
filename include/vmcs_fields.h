/* vmcs_fields.h
 * VMCS field encodings, per Intel SDM Vol 3C, Appendix B. Standard,
 * architecturally-defined values - not implementation-specific, so
 * these never need to change.
 */
#ifndef VMCS_FIELDS_H
#define VMCS_FIELDS_H

/* --- 16-bit fields --- */
#define GUEST_ES_SELECTOR   0x0800u
#define GUEST_CS_SELECTOR   0x0802u
#define GUEST_SS_SELECTOR   0x0804u
#define GUEST_DS_SELECTOR   0x0806u
#define GUEST_FS_SELECTOR   0x0808u
#define GUEST_GS_SELECTOR   0x080Au
#define GUEST_LDTR_SELECTOR 0x080Cu
#define GUEST_TR_SELECTOR   0x080Eu

#define HOST_ES_SELECTOR 0x0C00u
#define HOST_CS_SELECTOR 0x0C02u
#define HOST_SS_SELECTOR 0x0C04u
#define HOST_DS_SELECTOR 0x0C06u
#define HOST_FS_SELECTOR 0x0C08u
#define HOST_GS_SELECTOR 0x0C0Au
#define HOST_TR_SELECTOR 0x0C0Cu

/* --- 64-bit control fields --- */
/* IO bitmaps A and B: each is a 4KB page. Bit N set → port N exits.
 * Bitmap A covers ports 0x0000-0x7FFF; B covers 0x8000-0xFFFF. */
#define IO_BITMAP_A 0x2000u
#define IO_BITMAP_B 0x2002u
#define MSR_BITMAP  0x2004u
#define EPT_POINTER 0x201Au

/* --- 64-bit read-only data field ---
 * The guest-physical address an EPT violation/misconfiguration
 * happened on - the only field that reports it. */
#define GUEST_PHYSICAL_ADDRESS 0x2400u

/* --- 64-bit fields --- */
#define VMCS_LINK_POINTER   0x2800u
#define GUEST_IA32_DEBUGCTL 0x2802u
#define GUEST_IA32_PAT      0x2804u
#define GUEST_IA32_EFER     0x2806u

#define HOST_IA32_PAT  0x2C00u
#define HOST_IA32_EFER 0x2C02u

/* --- 32-bit control fields --- */
#define PIN_BASED_VM_EXEC_CONTROL     0x4000u
#define CPU_BASED_VM_EXEC_CONTROL     0x4002u /* "primary" processor-based controls */
#define EXCEPTION_BITMAP              0x4004u
#define CR3_TARGET_COUNT              0x400Au
#define VM_EXIT_CONTROLS              0x400Cu
#define VM_EXIT_MSR_STORE_COUNT       0x400Eu
#define VM_EXIT_MSR_LOAD_COUNT        0x4010u
#define VM_ENTRY_CONTROLS             0x4012u
#define VM_ENTRY_MSR_LOAD_COUNT       0x4014u
#define VM_ENTRY_INTR_INFO_FIELD      0x4016u
#define VM_ENTRY_EXCEPTION_ERROR_CODE 0x4018u
#define VM_ENTRY_INSTRUCTION_LEN      0x401Au
#define SECONDARY_VM_EXEC_CONTROL     0x401Eu

/* --- 32-bit read-only data fields --- */
#define VM_INSTRUCTION_ERROR      0x4400u
#define VM_EXIT_REASON            0x4402u
#define VM_EXIT_INTR_INFO         0x4404u
#define VM_EXIT_INTR_ERROR_CODE   0x4406u
/* An event that was being delivered through the guest's IDT when the
 * exit happened - it has to be re-injected, or it's simply lost. */
#define IDT_VECTORING_INFO_FIELD  0x4408u
#define IDT_VECTORING_ERROR_CODE  0x440Au
#define VM_EXIT_INSTRUCTION_LEN   0x440Cu
#define VMX_INSTRUCTION_INFO      0x440Eu

/* --- 32-bit guest-state fields --- */
#define GUEST_ES_LIMIT   0x4800u
#define GUEST_CS_LIMIT   0x4802u
#define GUEST_SS_LIMIT   0x4804u
#define GUEST_DS_LIMIT   0x4806u
#define GUEST_FS_LIMIT   0x4808u
#define GUEST_GS_LIMIT   0x480Au
#define GUEST_LDTR_LIMIT 0x480Cu
#define GUEST_TR_LIMIT   0x480Eu
#define GUEST_GDTR_LIMIT 0x4810u
#define GUEST_IDTR_LIMIT 0x4812u
#define GUEST_ES_AR_BYTES   0x4814u
#define GUEST_CS_AR_BYTES   0x4816u
#define GUEST_SS_AR_BYTES   0x4818u
#define GUEST_DS_AR_BYTES   0x481Au
#define GUEST_FS_AR_BYTES   0x481Cu
#define GUEST_GS_AR_BYTES   0x481Eu
#define GUEST_LDTR_AR_BYTES 0x4820u
#define GUEST_TR_AR_BYTES   0x4822u
#define GUEST_INTERRUPTIBILITY_INFO 0x4824u
#define GUEST_ACTIVITY_STATE        0x4826u
#define GUEST_SYSENTER_CS           0x482Au

/* --- 32-bit host-state field --- */
#define HOST_IA32_SYSENTER_CS 0x4C00u

/* --- Natural-width control fields ---
 * A guest/host mask bit of 1 means the host owns that CR bit: the
 * guest's reads of it are answered from the read shadow, and a guest
 * write that would change it causes a VM exit instead. A 0 bit belongs
 * to the guest outright - written straight through, no exit. */
#define CR0_GUEST_HOST_MASK 0x6000u
#define CR4_GUEST_HOST_MASK 0x6002u
#define CR0_READ_SHADOW     0x6004u
#define CR4_READ_SHADOW     0x6006u

/* --- Natural-width read-only fields --- */
#define EXIT_QUALIFICATION   0x6400u
#define GUEST_LINEAR_ADDRESS 0x640Au

/* --- Natural-width guest-state fields --- */
#define GUEST_CR0   0x6800u
#define GUEST_CR3   0x6802u
#define GUEST_CR4   0x6804u
#define GUEST_ES_BASE   0x6806u
#define GUEST_CS_BASE   0x6808u
#define GUEST_SS_BASE   0x680Au
#define GUEST_DS_BASE   0x680Cu
#define GUEST_FS_BASE   0x680Eu
#define GUEST_GS_BASE   0x6810u
#define GUEST_LDTR_BASE 0x6812u
#define GUEST_TR_BASE   0x6814u
#define GUEST_GDTR_BASE 0x6816u
#define GUEST_IDTR_BASE 0x6818u
#define GUEST_DR7   0x681Au
#define GUEST_RSP   0x681Cu
#define GUEST_RIP   0x681Eu
#define GUEST_RFLAGS 0x6820u
#define GUEST_PENDING_DBG_EXCEPTIONS 0x6822u
#define GUEST_SYSENTER_ESP 0x6824u
#define GUEST_SYSENTER_EIP 0x6826u

/* --- Natural-width host-state fields --- */
#define HOST_CR0 0x6C00u
#define HOST_CR3 0x6C02u
#define HOST_CR4 0x6C04u
#define HOST_FS_BASE 0x6C06u
#define HOST_GS_BASE 0x6C08u
#define HOST_TR_BASE 0x6C0Au
#define HOST_GDTR_BASE 0x6C0Cu
#define HOST_IDTR_BASE 0x6C0Eu
#define HOST_IA32_SYSENTER_ESP 0x6C10u
#define HOST_IA32_SYSENTER_EIP 0x6C12u
#define HOST_RSP 0x6C14u
#define HOST_RIP 0x6C16u

#endif
