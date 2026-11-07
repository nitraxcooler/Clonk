/*
 * exec_hello.c — Simple Ring 3 test program loaded from FAT32.
 *
 * Prints "Hello from HELLO.ELF!" and exits with status 0.
 * No libc. No runtime. Just raw syscalls.
 */
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
    static const char message[] = "Hello from HELLO.ELF!\n";
    (void)syscall2(CLONK_SYS_WRITE,
                   (uint64_t)(uintptr_t)message,
                   sizeof(message) - 1U);
    (void)syscall2(CLONK_SYS_EXIT, 0U, 0U);
    for (;;) __asm__ volatile ("pause");
}
