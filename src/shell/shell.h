#ifndef SHELL_H
#define SHELL_H

#include <stdint.h>

void shell_handle_command(const char *cmdline);

void shell_print(const char *s);
void shell_print_u64(uint64_t value);
void shell_print_hex(uint64_t value);

#endif
