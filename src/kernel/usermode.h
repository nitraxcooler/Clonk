#ifndef USERMODE_H
#define USERMODE_H

#include <stdint.h>

void usermode_run_init(void);
int64_t syscall_dispatch(const uint64_t *saved_registers);
void user_process_fault(uint64_t vector, uint64_t rip, uint64_t error_code);

#endif
