#include "kernel/usermode.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "abi/syscall.h"
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/io.h"
#include "boot/limine.h"
#include "drivers/keyboard/keyboard.h"
#include "fs/vfs.h"
#include "kernel/elf.h"
#include "kernel/heap.h"
#include "kernel/panic.h"
#include "kernel/process.h"
#include "kernel/usercopy.h"
#include "kernel/userfs.h"
#include "kernel/vmm.h"
#include "lib/mem.h"
#include "lib/string.h"

#define USER_OUTPUT_COLOR 0xFFFFFFU

extern volatile struct limine_module_request limine_module_request;
volatile uint64_t arch_saved_kernel_rsp;
volatile uint64_t arch_saved_kernel_rflags;
extern void kputc(char character, uint32_t color);
extern void kbackspace(void);
extern void kclear(void);

void arch_enter_user(uint64_t entry_point, uint64_t stack_pointer);
void arch_return_to_kernel(void) __attribute__((noreturn));

static void serial_write(const char *text) {
    while (*text != '\0') outb(0x3F8, (uint8_t)*text++);
}

static void serial_decimal(uint64_t value) {
    char digits[20];
    size_t count = 0U;
    do {
        digits[count++] = (char)('0' + value % 10U);
        value /= 10U;
    } while (value != 0U);
    while (count != 0U) outb(0x3F8, (uint8_t)digits[--count]);
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

static struct limine_file *find_module(const char *name) {
    if (limine_module_request.response == NULL) return NULL;
    struct limine_module_response *response = limine_module_request.response;
    for (uint64_t i = 0; i < response->module_count; ++i) {
        struct limine_file *module = response->modules[i];
        if (module != NULL &&
            (string_contains(module->path, name) ||
             string_contains(module->string, name))) return module;
    }
    return NULL;
}

static struct limine_file *find_init_module(void) {
    return find_module("init.elf");
}

static int64_t spawn_vfs_file(uint64_t address_space, uint64_t user_path) {
    char path[CLONK_FS_MAX_PATH];
    if (userfs_copy_path(address_space, user_path, path) != 0) {
        return CLONK_PROCESS_ERR_PATH;
    }

    vfs_file file;
    memset(&file, 0, sizeof(file));
    int rc = vfs_open(path, &file);
    if (rc != 0) {
        if (rc == VFS_ERR_NOT_FOUND) return CLONK_PROCESS_ERR_NOT_FOUND;
        if (rc == VFS_ERR_INVALID) return CLONK_PROCESS_ERR_PATH;
        if (rc == VFS_ERR_IS_DIRECTORY) {
            return CLONK_PROCESS_ERR_IS_DIRECTORY;
        }
        if (rc == VFS_ERR_NOT_DIRECTORY) return CLONK_PROCESS_ERR_PATH;
        if (rc == VFS_ERR_NO_MEMORY) return CLONK_PROCESS_ERR_NO_MEMORY;
        return CLONK_PROCESS_ERR_IO;
    }

    if (file.type != VFS_TYPE_FILE) {
        vfs_close(&file);
        return CLONK_PROCESS_ERR_IS_DIRECTORY;
    }
    if (file.size == 0U) {
        vfs_close(&file);
        return CLONK_PROCESS_ERR_INVALID;
    }
    if (file.size > PROCESS_EXECUTABLE_MAX_SIZE ||
        file.size > (uint64_t)SIZE_MAX) {
        vfs_close(&file);
        return CLONK_PROCESS_ERR_IMAGE_LIMIT;
    }

    size_t image_size = (size_t)file.size;
    uint8_t *image = (uint8_t *)kmalloc(image_size);
    if (image == NULL) {
        vfs_close(&file);
        return CLONK_PROCESS_ERR_NO_MEMORY;
    }

    uint64_t offset = 0U;
    bool read_ok = true;
    while (offset < file.size) {
        uint64_t request = file.size - offset;
        if (request > CLONK_FS_MAX_TRANSFER) request = CLONK_FS_MAX_TRANSFER;
        uint64_t received = 0U;
        rc = vfs_read(&file, offset, request, image + (size_t)offset, &received);
        if (rc != 0 || received == 0U || received > request ||
            received > file.size - offset) {
            read_ok = false;
            break;
        }
        offset += received;
    }

    vfs_close(&file);
    if (!read_ok || offset != (uint64_t)image_size) {
        kfree(image);
        return CLONK_PROCESS_ERR_IO;
    }

    int64_t result = process_spawn_image(image, image_size);
    kfree(image);
    return result;
}

void process_return_to_kernel(void) {
    arch_return_to_kernel();
}

void usermode_run_init(void) {
    struct limine_file *module = find_init_module();
    if (module == NULL || module->address == NULL || module->size == 0U) {
        kernel_panic("INIT_MODULE_MISSING");
    }

    uint64_t entry_point = 0U;
    uint64_t stack_pointer = 0U;
    uint64_t address_space = 0U;
    if (!elf_load_user(module->address, (size_t)module->size, &address_space,
                       &entry_point, &stack_pointer)) {
        kernel_panic("INIT_ELF_INVALID");
    }
    if (!process_manager_start_init(address_space)) {
        (void)vm_destroy_address_space(address_space);
        kernel_panic("INIT_PROCESS_CREATE_FAILED");
    }

    keyboard_set_user_mode(true);
    serial_write("USER: entering /init\n");
    vm_switch_address_space(address_space);
    arch_set_kernel_stack(arch_kernel_stack_top());
    arch_enter_user(entry_point, stack_pointer);
    keyboard_set_user_mode(false);
    serial_write("USER: /init exited; kernel resumed\n");
    process_reap_deferred_stacks();
}

int64_t syscall_dispatch(const uint64_t *saved_registers) {
    uint64_t number = saved_registers[14];
    uint64_t arg1 = saved_registers[9];
    uint64_t arg2 = saved_registers[10];
    uint64_t arg3 = saved_registers[11];
    uint64_t address_space = process_current_address_space();

    if (address_space == 0U || vm_get_active_pml4() != address_space) {
        kernel_panic("SYSCALL_WITHOUT_CURRENT_PROCESS");
    }
    if (number == CLONK_SYS_WRITE) {
        if (arg2 > 256U || !vm_user_range_valid(address_space, arg1, arg2)) {
            return -1;
        }
        char text[256];
        if (!user_copy_from(address_space, text, arg1, (size_t)arg2)) {
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
    if (number == CLONK_SYS_READ) {
        uint64_t flags;
        if (arg2 != 1U || !vm_user_range_valid(address_space, arg1, 1U) ||
            !vm_user_range_writable(address_space, arg1, 1U) ||
            !vm_get_page_flags_in(address_space, arg1, &flags) ||
            (flags & VMM_PAGE_USER) == 0U) {
            return -1;
        }
        char character;
        if (!keyboard_read_user_char(&character)) return -1;
        if (!user_copy_to(address_space, arg1, &character, sizeof(character))) {
            return -1;
        }
        return 1;
    }
    if (number == CLONK_SYS_CLEAR) {
        if (arg1 != 0U || arg2 != 0U) return -1;
        kclear();
        return 0;
    }
    if (number == CLONK_SYS_EXIT) {
        if (process_current_pid() == 1U) {
            if (!process_prepare_init_exit()) return -1;
            keyboard_set_user_mode(false);
            process_return_to_kernel();
        }
        if (!process_exit_current((int64_t)arg1)) return -1;
        return 0;
    }
    if (number == CLONK_SYS_SPAWN) {
        if (arg2 != 0U || arg3 != 0U) return -1;
        return spawn_vfs_file(address_space, arg1);
    }
    if (number == CLONK_SYS_WAIT) {
        if (arg3 != 0U) return -1;
        return process_wait(arg1, arg2);
    }
    if (number == CLONK_SYS_YIELD) {
        if (arg1 != 0U || arg2 != 0U || arg3 != 0U) return -1;
        process_request_yield();
        return 0;
    }
    if (number == CLONK_SYS_GETPID) {
        if (arg1 != 0U || arg2 != 0U || arg3 != 0U) return -1;
        return (int64_t)process_current_pid();
    }
    int64_t filesystem_result;
    if (userfs_handle_syscall(process_current_userfs(), number, arg1, arg2,
                              arg3, address_space, &filesystem_result)) {
        return filesystem_result;
    }
    return -1;
}

uint64_t *user_process_fault(uint64_t *saved_registers, uint64_t vector,
                             uint64_t rip, uint64_t error_code) {
    (void)rip;
    (void)error_code;
    uint64_t pid = process_current_pid();
    if (pid == 0U) kernel_panic("USER_EXCEPTION_WITHOUT_PROCESS");
    serial_write("USER: process ");
    serial_decimal(pid);
    serial_write(" terminated by exception ");
    serial_decimal(vector);
    serial_write("\n");
    (void)saved_registers;
    return process_terminate_current(-((int64_t)vector + 128));
}
