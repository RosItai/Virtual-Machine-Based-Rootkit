; paging.asm
; Builds a 4-level (PML4) page table structure that identity-maps the
; first MEM_SIZE_GB gigabytes of physical memory using 2MB pages, so
; that 3 levels (PML4 -> PDPT -> PD) are enough - no PT level needed.
;
; This only BUILDS the tables in memory - it does not turn paging on.
; Actually entering long mode needs, together: CR4.PAE=1, CR3 loaded
; with pml4_table's physical address, EFER.LME=1 (via the EFER MSR),
; then CR0.PG=1 - and after that, a far jump through a GDT with a
; 64-bit (L-bit) code segment to actually start executing 64-bit code.
; That's the "GDT" step in the README's roadmap, done separately.

bits 32

; --- Page table entry flags (Intel SDM Vol 3A, 4.5) ---
PTE_PRESENT     equ 1 << 0   ; entry points to a valid next-level table/page
PTE_WRITEABLE   equ 1 << 1   ; page/table is writable
PTE_PAGESIZE    equ 1 << 7   ; PD entry: this maps a 2MB page directly

LARGE_PAGE_SIZE equ 0x200000 ; 2MB - the span one PD entry covers with PTE_PAGESIZE set

MEM_SIZE_GB equ 4

section .bss
align 4096
pml4_table:
    resb 4096
pdpt_table:
    resb 4096
; One PD table per GB, laid out contiguously so PDPT entry i can just
; point at pd_tables + i*4096. Each reserved block above is a multiple
; of 4096 bytes, so every label here stays page-aligned automatically.
pd_tables:
    resb 4096 * MEM_SIZE_GB

section .text
global setup_paging
global pml4_table

setup_paging:
    ; ---- PML4[0] -> pdpt_table ----
    mov eax, pdpt_table
    or eax, PTE_PRESENT | PTE_WRITEABLE
    mov dword [pml4_table], eax
    mov dword [pml4_table + 4], 0      ; upper 32 bits of a long-mode PTE

    ; ---- PDPT[0 .. MEM_SIZE_GB-1] -> one dedicated PD table each ----
    mov eax, pd_tables                  ; PD table for GB 0
    mov ebx, pdpt_table                 ; next PDPT entry to write
    mov ecx, MEM_SIZE_GB
.map_pdpt:
    mov edx, eax
    or edx, PTE_PRESENT | PTE_WRITEABLE
    mov dword [ebx], edx
    mov dword [ebx + 4], 0
    add eax, 4096                       ; next PD table
    add ebx, 8                          ; next PDPT entry (8 bytes each)
    loop .map_pdpt

    ; ---- Every PD entry, across all MEM_SIZE_GB tables: one 2MB page ----
    mov ecx, MEM_SIZE_GB
    shl ecx, 9                          ; * 512 entries/table = total PD entries
    xor eax, eax                        ; first mapped physical address: 0
    mov edi, pd_tables
.map_pd:
    mov edx, eax
    or edx, PTE_PRESENT | PTE_WRITEABLE | PTE_PAGESIZE
    mov dword [edi], edx
    mov dword [edi + 4], 0
    add edi, 8
    add eax, LARGE_PAGE_SIZE
    loop .map_pd

    ret
