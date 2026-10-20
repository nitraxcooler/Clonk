#ifndef GDT_H
#define GDT_H

#include <stdint.h>

void arch_cpu_init(void);
void arch_set_kernel_stack(uint64_t stack_top);
uint64_t arch_kernel_stack_top(void);
void arch_enable_nx(void);
int arch_nx_enabled(void);

#endif
