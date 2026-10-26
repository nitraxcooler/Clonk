#include "abi/syscall.h"

#include <stddef.h>
#include <stdint.h>

#ifndef SCHED_LABEL
#define SCHED_LABEL 'A'
#endif

static volatile uint64_t private_counter;

static inline int64_t syscall0(uint64_t number) {
    register uint64_t rax __asm__("rax") = number;
    register uint64_t rdi __asm__("rdi") = 0U;
    register uint64_t rsi __asm__("rsi") = 0U;
    register uint64_t rdx __asm__("rdx") = 0U;
    __asm__ volatile ("int $0x80" : "+a"(rax) :
                      "D"(rdi), "S"(rsi), "d"(rdx) : "memory");
    return (int64_t)rax;
}

static inline int64_t syscall2(uint64_t number, uint64_t arg1, uint64_t arg2) {
    register uint64_t rax __asm__("rax") = number;
    register uint64_t rdi __asm__("rdi") = arg1;
    register uint64_t rsi __asm__("rsi") = arg2;
    register uint64_t rdx __asm__("rdx") = 0U;
    __asm__ volatile ("int $0x80" : "+a"(rax) :
                      "D"(rdi), "S"(rsi), "d"(rdx) : "memory");
    return (int64_t)rax;
}

static size_t append_decimal(char *buffer, size_t offset, uint64_t value) {
    char digits[20];
    size_t count = 0U;
    do {
        digits[count++] = (char)('0' + value % 10U);
        value /= 10U;
    } while (value != 0U);
    while (count != 0U) buffer[offset++] = digits[--count];
    return offset;
}

void _start(void) {
    uint64_t pid = (uint64_t)syscall0(CLONK_SYS_GETPID);
    volatile uint64_t private_stack_counter = 0U;
    (void)syscall0(CLONK_SYS_YIELD);
    for (uint64_t i = 0; i < 5U; ++i) {
        ++private_counter;
        ++private_stack_counter;
        if (private_counter != i + 1U || private_stack_counter != i + 1U) {
            (void)syscall2(CLONK_SYS_EXIT, 90U, 0U);
            for (;;) __asm__ volatile ("pause");
        }

        for (volatile uint64_t delay = 0U; delay < 2000000U; ++delay) {}
        char message[40];
        message[0] = 'S';
        message[1] = 'C';
        message[2] = 'H';
        message[3] = 'E';
        message[4] = 'D';
        message[5] = '-';
        message[6] = SCHED_LABEL;
        message[7] = ' ';
        message[8] = 'p';
        message[9] = 'i';
        message[10] = 'd';
        message[11] = '=';
        size_t length = append_decimal(message, 12U, pid);
        message[length++] = '\n';
        (void)syscall2(CLONK_SYS_WRITE, (uint64_t)(uintptr_t)message, length);
    }
    (void)syscall2(CLONK_SYS_EXIT, 0U, 0U);
    for (;;) __asm__ volatile ("pause");
}
