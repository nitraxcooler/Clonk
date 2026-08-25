#ifndef SHELL_H
#define SHELL_H

#include <stdint.h>

/*
 * MyOS command dispatcher.
 *
 * The keyboard driver feeds whole lines in here, one at a time. The
 * shell stays out of the kernel's internals every command just calls
 * a kernel API instead of poking around in its private business.
 */

void shell_handle_command(const char *cmdline);

/* Text helpers the commands use to talk to the screen and serial. */
void shell_print(const char *s);
void shell_print_u64(uint64_t value);
void shell_print_hex(uint64_t value);

#endif