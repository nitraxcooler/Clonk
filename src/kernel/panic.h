#ifndef PANIC_H
#define PANIC_H

#include <stdbool.h>
#include <stdint.h>

#include "boot/limine.h"

typedef struct {
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rip, cs, rflags;
} cpu_reg_frame;

typedef struct {
    bool present;
    bool write;
    bool user;
    bool reserved;
    bool instruction;
} pf_decode;

extern volatile uint64_t limine_base_revision[3];
extern volatile struct limine_framebuffer_request framebuffer_request;

void decode_page_error(uint64_t error, pf_decode *out);

void kernel_panic(const char *reason);
void kernel_panic_exception(const char *reason, uint8_t vector, const char *name,
                           uint64_t error_code, uint64_t rip, uint64_t cs,
                           uint64_t rflags, uint64_t rsp, uint64_t ss);
void kernel_panic_page_fault(const char *reason, uint8_t vector, const char *name,
                           uint64_t error_code, uint64_t fault_address,
                           uint64_t rip, uint64_t cs, uint64_t rflags,
                           uint64_t rsp, uint64_t ss,
                           const cpu_reg_frame *regs);

#endif
