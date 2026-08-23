#ifndef PANIC_H
#define PANIC_H

#include <stdbool.h>
#include <stdint.h>

#include "boot/limine.h"

/* CPU register snapshot captured by the ISR before it dispatched to the
 * exception handlers (saved with the exceptions further below).
 * All values are the machine registers *at the moment of the fault*. */
typedef struct {
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rip, cs, rflags;
} cpu_reg_frame;

/* Decoded x86-64 page-fault error-code bits (CR2 / cpu frame error code). */
typedef struct {
    bool present;     /* bit0: 1 = present (protection violation), 0 = not present */
    bool write;       /* bit1: 1 = write access, 0 = read */
    bool user;        /* bit2: 1 = user-mode, 0 = kernel/supervisor */
    bool reserved;    /* bit3: 1 = reserved-bit violation */
    bool instruction; /* bit4: 1 = instruction-fetch fault */
} pf_decode;

extern volatile uint64_t limine_base_revision[3];
extern volatile struct limine_framebuffer_request framebuffer_request;

/* Decode the page-fault error code into human-intelligible bits. */
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
