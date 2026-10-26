#ifndef CLONK_KERNEL_PROCESS_H
#define CLONK_KERNEL_PROCESS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kernel/userfs.h"

#define PROCESS_TABLE_LIMIT 8U

bool process_manager_start_init(uint64_t address_space);
uint64_t process_current_address_space(void);
uint64_t process_current_pid(void);
userfs_context *process_current_userfs(void);
int64_t process_spawn_image(const void *image, size_t image_size);
int64_t process_wait(uint64_t pid, uint64_t status_address);
void process_request_yield(void);
uint64_t *process_syscall_return(uint64_t *frame);
uint64_t *process_timer_interrupt(uint64_t *frame);
bool process_exit_current(int64_t status);
uint64_t *process_terminate_current(int64_t status);
bool process_prepare_init_exit(void);
void process_reap_deferred_stacks(void);
void process_return_to_kernel(void) __attribute__((noreturn));

#endif
