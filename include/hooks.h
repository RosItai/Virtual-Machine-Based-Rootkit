/* hooks.h - guest-facing hooks the VMM installs before launch.
 *
 * Right now there's one: the guest's BIOS memory-map call (INT 15h,
 * EAX=0xE820) is redirected so the guest sees the VMM's own memory as
 * reserved, and never loads anything on top of it. The redirection is
 * a tiny real-mode stub (int15_hook.asm) spliced into the guest's IVT;
 * it hands control to the host with a VMCALL, and the host does the
 * actual work here in C.
 */
#ifndef HOOKS_H
#define HOOKS_H

#include "vmexit.h"

/* The marker the hook stub leaves in EBP so the host can tell our
 * hypercall apart from a guest that executes VMCALL on its own (which
 * still gets a #UD). "VMBR" as four ASCII bytes. */
#define VMBR_HYPERCALL_MAGIC 0x564D4252u

/* The INT 13h (disk) hook stub uses its own marker, "VMBD", so the host
 * dispatches it to the disk observer instead of the E820 service. */
#define VMBR_DISK_HYPERCALL_MAGIC 0x564D4244u

/* Builds the filtered E820 map the guest will see (the real map with
 * the VMM's ranges carved out as reserved) and splices the INT 15h
 * hook stub into the guest's IVT. Call once, after every allocation is
 * done (so the reserved ranges are final) and after the guest has been
 * copied in, but before VMLAUNCH. */
void installGuestHooks(void);

/* Dispatches a VMCALL exit. Returns 1 if it was our hypercall (EBP ==
 * VMBR_HYPERCALL_MAGIC) and has been handled - RIP advanced, guest
 * registers set; returns 0 if it wasn't ours, so the caller injects a
 * #UD exactly as it would for any other VMX instruction. */
int handleHypercall(VmExitContext *ctx);

#endif
