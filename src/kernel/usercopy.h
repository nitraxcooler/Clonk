#ifndef MYOS_KERNEL_USERCOPY_H
#define MYOS_KERNEL_USERCOPY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool user_copy_from(uint64_t address_space, void *kernel_destination,
                    uint64_t user_source, size_t length);
bool user_copy_to(uint64_t address_space, uint64_t user_destination,
                  const void *kernel_source, size_t length);

#endif
