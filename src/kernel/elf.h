#ifndef ELF_H
#define ELF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool elf_load_user(const void *image, size_t image_size, uint64_t *address_space,
                   uint64_t *entry_point, uint64_t *stack_pointer);
void elf_self_test(void);

#endif
