#include "abi/syscall.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define INPUT_BUFFER_SIZE 128U

static char input_buffer[INPUT_BUFFER_SIZE];
static char read_validation_buffer;

static inline int64_t syscall2(uint64_t number, uint64_t arg1, uint64_t arg2) {
    register uint64_t rax __asm__("rax") = number;
    register uint64_t rdi __asm__("rdi") = arg1;
    register uint64_t rsi __asm__("rsi") = arg2;
    __asm__ volatile ("int $0x80" : "+a"(rax) : "D"(rdi), "S"(rsi) : "memory");
    return (int64_t)rax;
}

static int64_t syscall0(uint64_t number) {
    return syscall2(number, 0, 0);
}

static size_t text_length(const char *text) {
    size_t length = 0;
    while (text[length] != '\0') {
        ++length;
    }
    return length;
}

static bool strings_equal(const char *left, size_t left_length, const char *right) {
    size_t i = 0;
    while (i < left_length && right[i] != '\0' && left[i] == right[i]) {
        ++i;
    }
    return i == left_length && right[i] == '\0';
}

static bool write_text(const char *text) {
    size_t length = text_length(text);
    size_t written = 0;
    while (written < length) {
        size_t amount = length - written;
        if (amount > 256U) {
            amount = 256U;
        }
        if (syscall2(MYOS_SYS_WRITE, (uint64_t)(uintptr_t)(text + written), amount) != 0) {
            return false;
        }
        written += amount;
    }
    return true;
}

static bool write_character(char character) {
    return syscall2(MYOS_SYS_WRITE, (uint64_t)(uintptr_t)&character, 1U) == 0;
}

static int syscall_validation_self_test(void) {
    const uint64_t user_limit = 0x0000800000000000ULL;
    const uint64_t stack_top = 0x00007FFFFFFFE000ULL;
    static const char readable[] = "valid";

    if (syscall2(MYOS_SYS_WRITE, 0xFFFFFFFF80000000ULL, 1U) != -1 ||
        syscall2(MYOS_SYS_WRITE, 0x0000000000500000ULL, 1U) != -1 ||
        syscall2(MYOS_SYS_WRITE, user_limit - 1U, 2U) != -1 ||
        syscall2(MYOS_SYS_WRITE, user_limit - 1U, UINT64_MAX) != -1 ||
        syscall2(MYOS_SYS_WRITE, stack_top - 1U, 2U) != -1 ||
        syscall2(MYOS_SYS_WRITE, (uint64_t)(uintptr_t)readable, 257U) != -1 ||
        syscall2(MYOS_SYS_WRITE, 0x0000000000500000ULL, 0U) != 0 ||
        syscall2(MYOS_SYS_READ, 0x0000000000500000ULL, 1U) != -1 ||
        syscall2(MYOS_SYS_READ, (uint64_t)(uintptr_t)readable, 1U) != -1 ||
        syscall2(MYOS_SYS_READ, (uint64_t)(uintptr_t)&read_validation_buffer, 0U) != -1 ||
        syscall2(MYOS_SYS_CLEAR, 1U, 0U) != -1 ||
        syscall2(0xFFFFFFFFU, 0U, 0U) != -1) {
        return -1;
    }
    return 0;
}

static void print_help(void) {
    static const char help[] =
        "Commands:\n"
        "  help          Show this help\n"
        "  echo TEXT     Print TEXT\n"
        "  clear         Clear the console\n"
        "  version       Show the MyOS version\n"
        "  exit          Return to the kernel shell\n";
    (void)write_text(help);
}

static void run_command(char *line) {
    while (*line == ' ' || *line == '\t') {
        ++line;
    }
    if (*line == '\0') {
        return;
    }

    char *command = line;
    size_t command_length = 0;
    while (line[command_length] != '\0' &&
           line[command_length] != ' ' && line[command_length] != '\t') {
        ++command_length;
    }
    char *arguments = line + command_length;
    while (*arguments == ' ' || *arguments == '\t') {
        ++arguments;
    }

    if (strings_equal(command, command_length, "help")) {
        print_help();
    } else if (strings_equal(command, command_length, "echo")) {
        (void)write_text(arguments);
        (void)write_text("\n");
    } else if (strings_equal(command, command_length, "clear") &&
               *arguments == '\0') {
        (void)syscall0(MYOS_SYS_CLEAR);
    } else if (strings_equal(command, command_length, "version") &&
               *arguments == '\0') {
        (void)write_text("MyOS v0.1.1 (x86_64 Long Mode Bare-Metal)\n");
    } else if (strings_equal(command, command_length, "exit") &&
               *arguments == '\0') {
        (void)syscall0(MYOS_SYS_EXIT);
    } else if (strings_equal(command, command_length, "clear") ||
               strings_equal(command, command_length, "version") ||
               strings_equal(command, command_length, "exit")) {
        (void)write_text("Usage: command takes no arguments.\n");
    } else {
        (void)write_text("Command not found: ");
        (void)write_text(line);
        (void)write_text("\n");
    }
}

static void run_shell(void) {
    for (;;) {
        size_t length = 0;
        bool too_long = false;
        input_buffer[0] = '\0';
        (void)write_text("MyOS$ ");

        for (;;) {
            char character;
            if (syscall2(MYOS_SYS_READ, (uint64_t)(uintptr_t)&character, 1U) != 1) {
                (void)write_text("\nInput unavailable; returning to the kernel shell.\n");
                (void)syscall0(MYOS_SYS_EXIT);
                for (;;) __asm__ volatile ("pause");
            }

            if (character == '\n') {
                (void)write_text("\n");
                if (too_long) {
                    (void)write_text("Command too long; input discarded.\n");
                } else {
                    input_buffer[length] = '\0';
                    run_command(input_buffer);
                }
                break;
            }
            if (character == '\b') {
                if (too_long) {
                    too_long = false;
                    length = 0;
                    input_buffer[0] = '\0';
                    (void)write_text("\nMyOS$ ");
                } else if (length > 0U) {
                    input_buffer[--length] = '\0';
                    (void)write_character('\b');
                }
                continue;
            }
            if (character >= ' ' && character <= '~' && !too_long) {
                if (length < INPUT_BUFFER_SIZE - 1U) {
                    input_buffer[length++] = character;
                    input_buffer[length] = '\0';
                    (void)write_character(character);
                } else {
                    too_long = true;
                }
            }
        }
    }
}

void _start(void) {
    static const char validation_failed[] = "USER: syscall validation FAILED\n";
    static const char validation_passed[] = "USER: syscall validation passed.\n";
    static const char banner[] =
        "MyOS userspace shell\n"
        "Type help for available commands.\n";

    if (syscall_validation_self_test() != 0) {
        (void)write_text(validation_failed);
    } else {
        (void)write_text(validation_passed);
    }
#ifdef MYOS_TEST_SUPERVISOR_FAULT
    (void)*(volatile uint64_t *)(uintptr_t)0xFFFFFFFF80000000ULL;
#endif
    (void)write_text(banner);
    run_shell();
}
