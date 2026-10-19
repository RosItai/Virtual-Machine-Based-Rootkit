/* cpu.h
 * Thin wrappers around instructions C has no syntax for - MSR and
 * CR0/CR4 access. General-purpose, not VMX-specific (vmx.c is the
 * first user).
 */
#ifndef CPU_H
#define CPU_H

#include <stdint.h>

static inline void cpuid(uint32_t leaf, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx) {
    __asm__ volatile("cpuid"
                      : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                      : "a"(leaf), "c"(0));
}

static inline uint64_t readMsr(uint32_t msr) {
    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

static inline void writeMsr(uint32_t msr, uint64_t value) {
    uint32_t low = (uint32_t)value;
    uint32_t high = (uint32_t)(value >> 32);
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(low), "d"(high));
}

static inline uint64_t readCr0(void) {
    uint64_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    return cr0;
}

static inline void writeCr0(uint64_t cr0) {
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0));
}

static inline uint64_t readCr3(void) {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

static inline void writeCr3(uint64_t cr3) {
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3));
}

static inline uint64_t readCr4(void) {
    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    return cr4;
}

static inline void writeCr4(uint64_t cr4) {
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4));
}

static inline uint64_t readDr7(void) {
    uint64_t dr7;
    __asm__ volatile("mov %%dr7, %0" : "=r"(dr7));
    return dr7;
}

static inline uint64_t readRflags(void) {
    uint64_t rflags;
    __asm__ volatile("pushfq\n\t"
                      "pop %0"
                      : "=r"(rflags));
    return rflags;
}

/* Segment selector reads - each is just the 16-bit selector currently
 * loaded, not the descriptor it refers to. */
static inline uint16_t readCS(void) { uint16_t v; __asm__ volatile("mov %%cs, %0" : "=r"(v)); return v; }
static inline uint16_t readSS(void) { uint16_t v; __asm__ volatile("mov %%ss, %0" : "=r"(v)); return v; }
static inline uint16_t readDS(void) { uint16_t v; __asm__ volatile("mov %%ds, %0" : "=r"(v)); return v; }
static inline uint16_t readES(void) { uint16_t v; __asm__ volatile("mov %%es, %0" : "=r"(v)); return v; }
static inline uint16_t readFS(void) { uint16_t v; __asm__ volatile("mov %%fs, %0" : "=r"(v)); return v; }
static inline uint16_t readGS(void) { uint16_t v; __asm__ volatile("mov %%gs, %0" : "=r"(v)); return v; }

/* TR/LDTR aren't plain segment registers - STR/SLDT read them. */
static inline uint16_t readTR(void)   { uint16_t v; __asm__ volatile("str %0"  : "=r"(v)); return v; }
static inline uint16_t readLDTR(void) { uint16_t v; __asm__ volatile("sldt %0" : "=r"(v)); return v; }

static inline void loadTR(uint16_t selector) {
    __asm__ volatile("ltr %0" : : "r"(selector));
}

/* SGDT/SIDT fill a 10-byte pseudo-descriptor (2-byte limit + 8-byte
 * base in 64-bit mode). */
static inline uint64_t readGdtrBase(void) {
    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) descriptor;
    __asm__ volatile("sgdt %0" : "=m"(descriptor));
    return descriptor.base;
}

static inline uint16_t readGdtrLimit(void) {
    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) descriptor;
    __asm__ volatile("sgdt %0" : "=m"(descriptor));
    return descriptor.limit;
}

static inline uint64_t readIdtrBase(void) {
    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) descriptor;
    __asm__ volatile("sidt %0" : "=m"(descriptor));
    return descriptor.base;
}

static inline uint16_t readIdtrLimit(void) {
    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) descriptor;
    __asm__ volatile("sidt %0" : "=m"(descriptor));
    return descriptor.limit;
}

#endif
