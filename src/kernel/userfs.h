#ifndef MYOS_KERNEL_USERFS_H
#define MYOS_KERNEL_USERFS_H

#include <stdbool.h>
#include <stdint.h>

typedef struct userfs_context userfs_context;

userfs_context *userfs_create_context(void);
void userfs_destroy_context(userfs_context *context);
bool userfs_handle_syscall(userfs_context *context, uint64_t number,
                           uint64_t arg1, uint64_t arg2, uint64_t arg3,
                           uint64_t address_space, int64_t *result);

#endif
