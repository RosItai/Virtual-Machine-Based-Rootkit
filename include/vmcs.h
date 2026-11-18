/* vmcs.h
 * VMX capability MSRs, VM-execution/exit/entry control bits, control
 * register bits and VM-exit reasons - everything both the setup side
 * (vmx.c) and the exit-handling side (vmexit.c) need to agree on, plus
 * the two VMCS accessors themselves (implemented in vmx.c).
 *
 * Field *encodings* live in vmcs_fields.h, which this pulls in so one
 * include covers "talking to a VMCS".
 */
#ifndef VMCS_H
#define VMCS_H

#include <stdint.h>
#include "vmcs_fields.h"

/* --- VMX capability / feature MSRs --- */
#define IA32_FEATURE_CONTROL_MSR 0x3Au
#define FEATURE_CONTROL_LOCKED          (1ull << 0)
#define FEATURE_CONTROL_VMX_OUTSIDE_SMX (1ull << 2)

#define IA32_VMX_BASIC_MSR               0x480u
#define IA32_VMX_PINBASED_CTLS_MSR       0x481u
#define IA32_VMX_PROCBASED_CTLS_MSR      0x482u
#define IA32_VMX_EXIT_CTLS_MSR           0x483u
#define IA32_VMX_ENTRY_CTLS_MSR          0x484u
#define IA32_VMX_CR0_FIXED0_MSR          0x486u
#define IA32_VMX_CR0_FIXED1_MSR          0x487u
#define IA32_VMX_CR4_FIXED0_MSR          0x488u
#define IA32_VMX_CR4_FIXED1_MSR          0x489u
#define IA32_VMX_PROCBASED_CTLS2_MSR     0x48Bu /* secondary controls - no "true" variant exists */
#define IA32_VMX_TRUE_PINBASED_CTLS_MSR  0x48Du
#define IA32_VMX_TRUE_PROCBASED_CTLS_MSR 0x48Eu
#define IA32_VMX_TRUE_EXIT_CTLS_MSR      0x48Fu
#define IA32_VMX_TRUE_ENTRY_CTLS_MSR     0x490u

/* --- Other MSRs the VMCS mirrors --- */
#define IA32_EFER_MSR    0xC0000080u
#define IA32_FS_BASE_MSR 0xC0000100u
#define IA32_GS_BASE_MSR 0xC0000101u

#define EFER_SCE (1ull << 0)
#define EFER_LME (1ull << 8)  /* Long Mode Enable - the guest sets this itself */
#define EFER_LMA (1ull << 10) /* Long Mode Active - must agree with CR0.PG and
                               * the IA32e-mode-guest entry control */
#define EFER_NXE (1ull << 11)

/* --- Control register bits --- */
#define CR0_PE (1ull << 0)
#define CR0_MP (1ull << 1)
#define CR0_EM (1ull << 2)
#define CR0_TS (1ull << 3)
#define CR0_ET (1ull << 4)
#define CR0_NE (1ull << 5)
#define CR0_PG (1ull << 31)

#define CR4_PAE     (1ull << 5)
#define CR4_VMXE    (1ull << 13)
#define CR4_OSXSAVE (1ull << 18)

/* --- Pin-based VM-execution controls --- */
#define PIN_EXTERNAL_INTERRUPT_EXITING (1u << 0)
#define PIN_NMI_EXITING                (1u << 3)

/* --- Primary processor-based VM-execution controls --- */
#define CPU_BASED_HLT_EXITING       (1u << 7)
#define CPU_BASED_INVLPG_EXITING    (1u << 9)
#define CPU_BASED_CR3_LOAD_EXITING  (1u << 15)
#define CPU_BASED_CR3_STORE_EXITING (1u << 16)
#define CPU_BASED_MOV_DR_EXITING    (1u << 23)
#define CPU_BASED_UNCOND_IO_EXITING (1u << 24)
#define CPU_BASED_USE_IO_BITMAPS    (1u << 25)
#define CPU_BASED_USE_MSR_BITMAPS   (1u << 28)
#define ACTIVATE_SECONDARY_CONTROLS (1u << 31)

/* --- Secondary processor-based VM-execution controls ---
 * The three "enable" bits below are not optional for a real guest OS:
 * without them the CPU raises #UD in the guest on RDTSCP, INVPCID and
 * XSAVES/XRSTORS respectively - instructions Windows genuinely
 * executes. The failure looks like a guest crash, not like a missing
 * control bit, which is what makes it worth forcing on deliberately. */
#define SECONDARY_ENABLE_EPT         (1u << 1)
#define SECONDARY_ENABLE_RDTSCP      (1u << 3)
#define SECONDARY_ENABLE_VPID        (1u << 5)
/* Lets the guest run with CR0.PE=0 or CR0.PG=0 (real mode, or
 * protected mode without paging) - exempts those two bits from the
 * CR0 FIXED0 check. VM-entry requires ENABLE_EPT alongside it, since
 * an unpaged guest's "physical" addresses must still be translated. */
#define SECONDARY_UNRESTRICTED_GUEST (1u << 7)
#define SECONDARY_ENABLE_INVPCID     (1u << 12)
#define SECONDARY_ENABLE_XSAVES      (1u << 20)

/* --- VM-exit controls --- */
#define VM_EXIT_SAVE_IA32_PAT        (1u << 18)
#define VM_EXIT_LOAD_IA32_PAT        (1u << 19)
/* Save the guest's EFER into GUEST_IA32_EFER on every exit, and reload
 * the host's from HOST_IA32_EFER. The save side is what lets the exit
 * handler read the guest's current EFER.LME at all (by then the
 * physical MSR already holds the host's value); the load side keeps
 * guest writes to EFER - NXE in particular - from leaking into the
 * host, which only gets LME/LMA fixed up for it automatically. */
#define VM_EXIT_SAVE_IA32_EFER       (1u << 20)
#define VM_EXIT_LOAD_IA32_EFER       (1u << 21)
#define VM_EXIT_HOST_ADDR_SPACE_SIZE (1u << 9)

/* --- VM-entry controls --- */
/* Set when the guest is running in IA-32e mode. Unlike every other
 * control here this one can't be decided once at init: the guest
 * enables long mode itself (EFER.LME, then CR0.PG), and VM-entry
 * rejects any entry where this bit, CR0.PG and EFER.LMA disagree -
 * see syncLongModeEntryControl in vmexit.c. */
#define VM_ENTRY_IA32E_MODE_GUEST (1u << 9)
#define VM_ENTRY_LOAD_IA32_PAT    (1u << 14)
/* Load GUEST_IA32_EFER on entry. Without it the guest would keep the
 * host's EFER.LME=1 - harmless for a real-mode guest that never turns
 * paging on, but not the clean reset state a real guest OS expects. */
#define VM_ENTRY_LOAD_IA32_EFER   (1u << 15)

/* --- VM-exit reasons (SDM Vol 3C, Appendix C) ---
 * Bits 15:0 of VM_EXIT_REASON; bit 31 marks a VM-entry failure rather
 * than a genuine exit. */
#define VM_EXIT_REASON_MASK          0xFFFFu
#define VM_EXIT_REASON_ENTRY_FAILURE (1ull << 31)

#define EXIT_REASON_EXCEPTION_NMI      0u
#define EXIT_REASON_EXTERNAL_INTERRUPT 1u
#define EXIT_REASON_TRIPLE_FAULT       2u
#define EXIT_REASON_INIT_SIGNAL        3u
#define EXIT_REASON_SIPI               4u
#define EXIT_REASON_INTERRUPT_WINDOW   7u
#define EXIT_REASON_NMI_WINDOW         8u
#define EXIT_REASON_TASK_SWITCH        9u
#define EXIT_REASON_CPUID              10u
#define EXIT_REASON_GETSEC             11u
#define EXIT_REASON_HLT                12u
#define EXIT_REASON_INVD               13u
#define EXIT_REASON_INVLPG             14u
#define EXIT_REASON_RDPMC              15u
#define EXIT_REASON_RDTSC              16u
#define EXIT_REASON_RSM                17u
#define EXIT_REASON_VMCALL             18u
#define EXIT_REASON_VMCLEAR            19u
#define EXIT_REASON_VMLAUNCH           20u
#define EXIT_REASON_VMPTRLD            21u
#define EXIT_REASON_VMPTRST            22u
#define EXIT_REASON_VMREAD             23u
#define EXIT_REASON_VMRESUME           24u
#define EXIT_REASON_VMWRITE            25u
#define EXIT_REASON_VMXOFF             26u
#define EXIT_REASON_VMXON              27u
#define EXIT_REASON_CR_ACCESS          28u
#define EXIT_REASON_DR_ACCESS          29u
#define EXIT_REASON_IO_INSTRUCTION     30u
#define EXIT_REASON_RDMSR              31u
#define EXIT_REASON_WRMSR              32u
#define EXIT_REASON_ENTRY_INVALID_GUEST_STATE 33u
#define EXIT_REASON_ENTRY_MSR_LOADING  34u
#define EXIT_REASON_MWAIT              36u
#define EXIT_REASON_MONITOR_TRAP_FLAG  37u
#define EXIT_REASON_MONITOR            39u
#define EXIT_REASON_PAUSE              40u
#define EXIT_REASON_ENTRY_MCE          41u
#define EXIT_REASON_TPR_BELOW_THRESHOLD 43u
#define EXIT_REASON_APIC_ACCESS        44u
#define EXIT_REASON_GDTR_IDTR_ACCESS   46u
#define EXIT_REASON_LDTR_TR_ACCESS     47u
#define EXIT_REASON_EPT_VIOLATION      48u
#define EXIT_REASON_EPT_MISCONFIG      49u
#define EXIT_REASON_INVEPT             50u
#define EXIT_REASON_RDTSCP             51u
#define EXIT_REASON_PREEMPTION_TIMER   52u
#define EXIT_REASON_INVVPID            53u
#define EXIT_REASON_WBINVD             54u
#define EXIT_REASON_XSETBV             55u
#define EXIT_REASON_APIC_WRITE         56u
#define EXIT_REASON_RDRAND             57u
#define EXIT_REASON_INVPCID            58u
#define EXIT_REASON_VMFUNC             59u
#define EXIT_REASON_XSAVES             63u
#define EXIT_REASON_XRSTORS            64u

/* --- Guest/host segment access-rights bits ---
 * The VMCS's own 32-bit AR format, NOT the same bit positions as a raw
 * GDT descriptor byte (see src/boot/macros.asm for that unrelated
 * set). Bits 0-7 match a standard descriptor access byte; bits 12-15
 * match a standard flags nibble, just shifted to sit directly in this
 * one field instead of split across descriptor bytes; bit 16 is
 * VMX-specific (Unusable). */
#define SEG_A         (1u << 0)   /* Accessed */
#define SEG_RW        (1u << 1)   /* Readable(code) / Writable(data) */
#define SEG_DC        (1u << 2)   /* Direction/Conforming */
#define SEG_E         (1u << 3)   /* Executable */
#define SEG_S         (1u << 4)   /* Descriptor type: 1=code/data, 0=system */
#define SEG_P         (1u << 7)   /* Present */
#define SEG_LONG_FLAG (1u << 13)  /* L - 64-bit code segment */
#define SEG_SIZE_FLAG (1u << 14)  /* D/B */
#define SEG_FLAG_G    (1u << 15)  /* Granularity */
#define SEG_UNUSABLE  (1u << 16)  /* VMX-specific: segment not in use */

/* --- Exit qualification layout for EXIT_REASON_CR_ACCESS --- */
#define CR_ACCESS_CR_NUMBER(qualification)  ((uint32_t)((qualification) & 0xFu))
#define CR_ACCESS_TYPE(qualification)       ((uint32_t)(((qualification) >> 4) & 0x3u))
#define CR_ACCESS_GPR(qualification)        ((uint32_t)(((qualification) >> 8) & 0xFu))
#define CR_ACCESS_LMSW_SOURCE(qualification) ((uint64_t)(((qualification) >> 16) & 0xFFFFu))

#define CR_ACCESS_TYPE_MOV_TO_CR   0u
#define CR_ACCESS_TYPE_MOV_FROM_CR 1u
#define CR_ACCESS_TYPE_CLTS        2u
#define CR_ACCESS_TYPE_LMSW        3u

/* --- VM-entry interruption information (for event injection) --- */
#define INTR_TYPE_EXTERNAL_INTERRUPT (0u << 8)
#define INTR_TYPE_NMI                (2u << 8)
#define INTR_TYPE_HARDWARE_EXCEPTION (3u << 8)
#define INTR_TYPE_SOFTWARE_INTERRUPT (4u << 8)
#define INTR_INFO_DELIVER_ERROR_CODE (1u << 11)
#define INTR_INFO_VALID              (1u << 31)

#define EXCEPTION_VECTOR_UD 6u  /* #UD - invalid opcode */
#define EXCEPTION_VECTOR_GP 13u /* #GP - general protection */

/* Blocking by STI (bit 0) and by MOV SS (bit 1) in
 * GUEST_INTERRUPTIBILITY_INFO. */
#define INTERRUPTIBILITY_BLOCKING_MASK 0x3ull

/* --- How every VMX instruction reports failure ---
 * CF=1 (VMfailInvalid) or ZF=1 (VMfailValid) means failure, both clear
 * means success. */
#define CARRY_FLAG_MASK (1ull << 0)
#define ZERO_FLAG_MASK  (1ull << 6)

/* VMCS accessors, implemented in vmx.c. vmwrite logs the field on
 * failure rather than returning a status - a failed VMWRITE always
 * means a wrong field encoding or an out-of-range value, i.e. a bug
 * here, not a condition any caller could sensibly recover from. */
uint64_t vmread(uint64_t field);
void vmwrite(uint64_t field, uint64_t value);

#endif
