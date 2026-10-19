#include "kernel/usermode.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "abi/syscall.h"
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/io.h"
#include "boot/limine.h"
#include "drivers/keyboard/keyboard.h"
#include "kernel/elf.h"
#include "kernel/panic.h"
#include "kernel/usercopy.h"
#include "kernel/userfs.h"
#include "kernel/vmm.h"
#include "lib/mem.h"

extern void kputc(char c, uint32_t fg_color);
extern void kbackspace(void);
extern void kclear(void);

#define USER_OUTPUT_COLOR 0xFFFFFFU

extern volatile struct limine_module_request limine_module_request;

volatile uint64_t arch_saved_kernel_rsp;
volatile uint64_t arch_saved_kernel_rflags;

static uint64_t kernel_address_space;
static uint64_t process_address_space;
static bool process_active;

extern void arch_enter_user(uint64_t entry_point, uint64_t stack_pointer);
extern void arch_return_to_kernel(void) __attribute__((noreturn));

static void serial_write(const char *text) {
    while (*text != '\0') outb(0x3F8, (uint8_t)*text++);
}

static bool string_contains(const char *text, const char *needle) {
    if (text == NULL || needle == NULL) return false;
    for (; *text != '\0'; ++text) {
        const char *a = text;
        const char *b = needle;
        while (*a != '\0' && *b != '\0' && *a == *b) {
            ++a;
            ++b;
        }
        if (*b == '\0') return true;
    }
    return false;
}

static struct limine_file *find_init_module(void) {
    if (limine_module_request.response == NULL) return NULL;
    struct limine_module_response *response = limine_module_request.response;
    for (uint64_t i = 0; i < response->module_count; ++i) {
        struct limine_file *module = response->modules[i];
        if (module != NULL &&
            (string_contains(module->path, "init.elf") ||
             string_contains(module->string, "init.elf"))) return module;
    }
    return NULL;
}

static void process_finish(const char *message) __attribute__((noreturn));
static void process_finish(const char *message) {
    serial_write(message);
    if (!process_active) kernel_panic("USER_PROCESS_STATE_INVALID");
    userfs_close_all();
    uint64_t dead_address_space = process_address_space;
    process_active = false;
    process_address_space = 0;
    vm_switch_address_space(kernel_address_space);
    if (!vm_destroy_address_space(dead_address_space)) {
        kernel_panic("USER_ADDRESS_SPACE_DESTROY_FAILED");
    }
    arch_return_to_kernel();
}

void usermode_run_init(void) {
    struct limine_file *module = find_init_module();
    if (module == NULL || module->address == NULL || module->size == 0U) {
        kernel_panic("INIT_MODULE_MISSING");
    }

    uint64_t entry_point = 0;
    uint64_t stack_pointer = 0;
    kernel_address_space = vm_get_active_pml4();
    if (kernel_address_space == 0U ||
        !elf_load_user(module->address, (size_t)module->size, &process_address_space,
                       &entry_point, &stack_pointer)) {
        kernel_panic("INIT_ELF_INVALID");
    }

    userfs_initialize();
    process_active = true;
    keyboard_set_user_mode(true);
    serial_write("USER: entering /init\n");
    vm_switch_address_space(process_address_space);
    arch_enter_user(entry_point, stack_pointer);
    if (process_active) process_finish("USER: /init returned without exit syscall\n");
    keyboard_set_user_mode(false);
    serial_write("USER: /init exited; kernel resumed\n");
}

int64_t syscall_dispatch(const uint64_t *saved_registers) {
    uint64_t number = saved_registers[14];
    uint64_t arg1 = saved_registers[9];
    uint64_t arg2 = saved_registers[10];
    uint64_t arg3 = saved_registers[11];

    if (!process_active) {
        kernel_panic("SYSCALL_WITHOUT_USER_PROCESS");
    }
    if (number == MYOS_SYS_WRITE) {
        if (arg2 > 256U || !vm_user_range_valid(process_address_space, arg1, arg2)) {
            return -1;
        }
        char text[256];
        if (!user_copy_from(process_address_space, text, arg1, (size_t)arg2)) {
            return -1;
        }
        for (uint64_t i = 0; i < arg2; ++i) {
            if (text[i] == '\b') {
                outb(0x3F8, '\b');
                kbackspace();
            } else {
                outb(0x3F8, (uint8_t)text[i]);
                kputc(text[i], USER_OUTPUT_COLOR);
            }
        }
        return 0;
    }
    if (number == MYOS_SYS_READ) {
        uint64_t flags;
        if (arg2 != 1U || !vm_user_range_valid(process_address_space, arg1, 1U) ||
            !vm_user_range_writable(process_address_space, arg1, 1U) ||
            !vm_get_page_flags_in(process_address_space, arg1, &flags) ||
            (flags & VMM_PAGE_USER) == 0U) {
            return -1;
        }
        char character;
        if (!keyboard_read_user_char(&character)) {
            return -1;
        }
        if (!user_copy_to(process_address_space, arg1, &character, sizeof(character))) {
            return -1;
        }
        return 1;
    }
    if (number == MYOS_SYS_CLEAR) {
        if (arg1 != 0U || arg2 != 0U) {
            return -1;
        }
        kclear();
        return 0;
    }
    if (number == MYOS_SYS_EXIT) {
        process_finish("USER: /init requested exit\n");
    }
    int64_t filesystem_result;
    if (userfs_handle_syscall(number, arg1, arg2, arg3,
                              process_address_space, &filesystem_result)) {
        return filesystem_result;
    }
    return -1;
}

void user_process_fault(uint64_t vector, uint64_t rip, uint64_t error_code) {
    (void)rip;
    (void)error_code;
    if (!process_active) kernel_panic("USER_EXCEPTION_WITHOUT_PROCESS");
    serial_write("USER: /init terminated by exception ");
    outb(0x3F8, (uint8_t)('0' + (vector / 10U)));
    outb(0x3F8, (uint8_t)('0' + (vector % 10U)));
    outb(0x3F8, '\n');
    process_finish("");
}
