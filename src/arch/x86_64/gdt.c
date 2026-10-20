#include "arch/x86_64/gdt.h"

#include <stddef.h>
#include <stdint.h>

struct tss64 {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

struct gdt_pointer {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static uint64_t gdt[7] __attribute__((aligned(16)));
static struct tss64 tss __attribute__((aligned(16)));
static uint8_t ring0_stack[16384] __attribute__((aligned(16)));
static int nx_supported;
static int nx_enabled;

extern void arch_load_gdt(const struct gdt_pointer *pointer);

static uint64_t tss_descriptor(uint64_t base, uint32_t limit) {
    return (uint64_t)(limit & 0xFFFFU) |
           ((base & 0xFFFFFFULL) << 16) |
           (0x89ULL << 40) |
           ((uint64_t)((limit >> 16) & 0x0FU) << 48) |
           (((base >> 24) & 0xFFULL) << 56);
}

void arch_set_kernel_stack(uint64_t stack_top) {
    tss.rsp0 = stack_top;
}

uint64_t arch_kernel_stack_top(void) {
    return (uint64_t)(uintptr_t)(ring0_stack + sizeof(ring0_stack));
}

int arch_nx_enabled(void) {
    return nx_enabled;
}

void arch_enable_nx(void) {
    if (!nx_supported || nx_enabled) return;
    uint32_t msr_low, msr_high;
    __asm__ volatile ("rdmsr" : "=a"(msr_low), "=d"(msr_high) : "c"(0xC0000080U));
    msr_low |= 1U << 11;
    __asm__ volatile ("wrmsr" : : "a"(msr_low), "d"(msr_high), "c"(0xC0000080U));
    nx_enabled = 1;
}

void arch_cpu_init(void) {
    __asm__ volatile ("cli");
    gdt[0] = 0;
    gdt[1] = 0x00AF9A000000FFFFULL;
    gdt[2] = 0x00CF92000000FFFFULL;
    gdt[3] = 0x00CFF2000000FFFFULL;
    gdt[4] = 0x00AFFA000000FFFFULL;
    tss.rsp0 = (uint64_t)(uintptr_t)(ring0_stack + sizeof(ring0_stack));
    tss.iomap_base = sizeof(tss);
    gdt[5] = tss_descriptor((uint64_t)(uintptr_t)&tss, sizeof(tss) - 1U);
    gdt[6] = (uint64_t)(uintptr_t)&tss >> 32;

    struct gdt_pointer pointer = {
        .limit = (uint16_t)(sizeof(gdt) - 1U),
        .base = (uint64_t)(uintptr_t)gdt
    };
    arch_load_gdt(&pointer);
    uint16_t tss_selector = 0x28;
    __asm__ volatile ("ltr %0" : : "r"(tss_selector));

    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                      : "a"(0x80000000U), "c"(0U));
    if (eax >= 0x80000001U) {
        __asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                          : "a"(0x80000001U), "c"(0U));
        if ((edx & (1U << 20)) != 0U) {
            nx_supported = 1;
        }
    }
}
