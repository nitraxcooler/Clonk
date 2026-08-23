#ifndef SHELL_H
#define SHELL_H

#include <stdint.h>

/*
 * MyOS CLI command dispatcher.
 *
 * The keyboard driver collects a line of input and hands it to
 * shell_handle_command(). This keeps the shell separate from kernel
 * internals: commands call kernel APIs (PMM/VMM/heap/...) rather than
 * poking at their private data.
 */

void shell_handle_command(const char *cmdline);

/* Low-level text helpers shared by commands (screen + serial). */
void shell_print(const char *s);
void shell_print_u64(uint64_t value);
void shell_print_hex(uint64_t value);

#endif