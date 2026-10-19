#ifndef MYOS_KERNEL_USERFS_H
#define MYOS_KERNEL_USERFS_H

#include <stdbool.h>
#include <stdint.h>

void userfs_initialize(void);
void userfs_close_all(void);
bool userfs_handle_syscall(uint64_t number, uint64_t arg1, uint64_t arg2,
                          uint64_t arg3, uint64_t address_space,
                          int64_t *result);

#endif
