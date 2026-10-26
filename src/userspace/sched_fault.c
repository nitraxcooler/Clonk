#include "abi/syscall.h"

#include <stdint.h>

static inline int64_t syscall2(uint64_t number, uint64_t arg1, uint64_t arg2) {
    register uint64_t rax __asm__("rax") = number;
    register uint64_t rdi __asm__("rdi") = arg1;
    register uint64_t rsi __asm__("rsi") = arg2;
    register uint64_t rdx __asm__("rdx") = 0U;
    __asm__ volatile ("int $0x80" : "+a"(rax) :
                      "D"(rdi), "S"(rsi), "d"(rdx) : "memory");
    return (int64_t)rax;
}

void _start(void) {
    static const char message[] = "SCHED-FAULT\n";
    (void)syscall2(CLONK_SYS_WRITE, (uint64_t)(uintptr_t)message,
                   sizeof(message) - 1U);
    __asm__ volatile ("ud2");
    for (;;) __asm__ volatile ("pause");
}
