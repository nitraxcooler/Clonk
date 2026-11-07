#ifndef CLONK_KERNEL_USERFS_H
#define CLONK_KERNEL_USERFS_H

#include <stdbool.h>
#include <stdint.h>

#include "abi/syscall.h"

typedef struct userfs_context userfs_context;

userfs_context *userfs_create_context(void);
void userfs_destroy_context(userfs_context *context);
int userfs_copy_path(uint64_t address_space, uint64_t user_path,
                     char path[CLONK_FS_MAX_PATH]);
bool userfs_handle_syscall(userfs_context *context, uint64_t number,
                           uint64_t arg1, uint64_t arg2, uint64_t arg3,
                           uint64_t address_space, int64_t *result);

#endif
