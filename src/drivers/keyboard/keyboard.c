#include <stdint.h>
#include "arch/x86_64/io.h"
#include "kernel/panic.h"
#include "kernel/pmm.h"
#include "kernel/vmm.h"
#include "lib/string.h"

extern void kprint(const char *str, uint32_t fg_color);
extern void kputc(char c, uint32_t fg_color);
extern void kbackspace(void);
extern void kclear(void);

static void kprint_u64(uint64_t value, uint32_t fg_color) {
    char digits[32];
    int length = 0;

    if (value == 0) {
        kputc('0', fg_color);
        return;
    }

    while (value > 0) {
        digits[length++] = (char)('0' + (value % 10U));
        value /= 10U;
    }

    for (int i = length - 1; i >= 0; --i) {
        kputc(digits[i], fg_color);
    }
}

static void kprint_hex64(uint64_t value, uint32_t fg_color) {
    static const char digits[] = "0123456789ABCDEF";
    kprint("0x", fg_color);
    for (int shift = 60; shift >= 0; shift -= 4) {
        kputc(digits[(value >> shift) & 0xF], fg_color);
    }
}

#define CMD_BUFFER_SIZE 256
static char cmd_buffer[CMD_BUFFER_SIZE];
static size_t cmd_len = 0;

static const char scancode_ascii[128] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
  '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
     0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
     0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
   '*',   0, ' '
};

void keyboard_init(void) {
    while (inb(0x64) & 2);
    outb(0x64, 0xAE); 

    while (inb(0x64) & 1) {
        inb(0x60);
    }
}

static void parse_command(const char *cmd) {
    if (strlen(cmd) == 0) {
        return;
    }

    if (strcmp(cmd, "help") == 0) {
        kprint("Available commands:\n", 0xFFFF00);
        kprint("  help    - Display this menu\n", 0xFFFFFF);
        kprint("  clear   - Clear the screen\n", 0xFFFFFF);
        kprint("  version - Display MyOS kernel version\n", 0xFFFFFF);
        kprint("  echo    - Print back text (e.g. echo hello)\n", 0xFFFFFF);
        kprint("  mem     - Display physical memory statistics\n", 0xFFFFFF);
        kprint("  vmem    - Display virtual memory statistics\n", 0xFFFFFF);
        kprint("  panic   - Trigger a deliberate kernel panic\n", 0xFFFFFF);
        kprint("  pfault  - Trigger a controlled page fault\n", 0xFFFFFF);
    } else if (strcmp(cmd, "clear") == 0) {
        kclear();
    } else if (strcmp(cmd, "version") == 0) {
        kprint("MyOS v0.1.0 (x86_64 Long Mode Bare-Metal)\n", 0x00FF00);
    } else if (strcmp(cmd, "mem") == 0) {
        if (!pmm_is_initialized()) {
            kprint("Memory manager not initialized.\n", 0xFF0000);
            return;
        }

        kprint("MyOS Memory Information\n", 0x00FF00);
        kprint("Page size:       ", 0xFFFFFF);
        kprint_u64(PMM_PAGE_SIZE, 0xFFFFFF);
        kprint(" bytes\n", 0xFFFFFF);

        kprint("Total memory:    ", 0xFFFFFF);
        kprint_u64(pmm_get_total_memory() / (1024ULL * 1024ULL), 0xFFFFFF);
        kprint(" MiB\n", 0xFFFFFF);

        kprint("Usable memory:   ", 0xFFFFFF);
        kprint_u64(pmm_get_usable_memory() / (1024ULL * 1024ULL), 0xFFFFFF);
        kprint(" MiB\n", 0xFFFFFF);

        kprint("Reserved memory: ", 0xFFFFFF);
        kprint_u64(pmm_get_reserved_memory() / (1024ULL * 1024ULL), 0xFFFFFF);
        kprint(" MiB\n", 0xFFFFFF);

        kprint("Total pages:     ", 0xFFFFFF);
        kprint_u64(pmm_get_total_pages(), 0xFFFFFF);
        kprint("\n", 0xFFFFFF);

        kprint("Free pages:      ", 0xFFFFFF);
        kprint_u64(pmm_get_free_pages(), 0xFFFFFF);
        kprint("\n", 0xFFFFFF);

        kprint("Used pages:      ", 0xFFFFFF);
        kprint_u64(pmm_get_used_pages(), 0xFFFFFF);
        kprint("\n", 0xFFFFFF);
    } else if (strcmp(cmd, "vmem") == 0) {
        if (!vm_is_initialized()) {
            kprint("Virtual memory manager not initialized.\n", 0xFF0000);
            return;
        }

        kprint("MyOS Virtual Memory Information\n", 0x00FF00);
        kprint("Page size:       ", 0xFFFFFF);
        kprint_u64(VMM_PAGE_SIZE, 0xFFFFFF);
        kprint(" bytes\n", 0xFFFFFF);
        kprint("Paging:          ENABLED\n", 0xFFFFFF);
        kprint("Current CR3:     ", 0xFFFFFF);
        kprint_hex64(vm_get_cr3(), 0xFFFFFF);
        kprint("\n", 0xFFFFFF);
        kprint("PML4:            ", 0xFFFFFF);
        kprint_hex64(vm_get_active_pml4(), 0xFFFFFF);
        kprint("\n", 0xFFFFFF);
        kprint("Mapped pages:    ", 0xFFFFFF);
        kprint_u64(vm_get_mapped_pages(), 0xFFFFFF);
        kprint("\n", 0xFFFFFF);
        kprint("Page tables:     ", 0xFFFFFF);
        kprint_u64(vm_get_page_table_count(), 0xFFFFFF);
        kprint("\n", 0xFFFFFF);
    } else if (strcmp(cmd, "panic") == 0) {
        kernel_panic("Manual panic requested.");
    } else if (strcmp(cmd, "pfault") == 0) {
        /* Development-only controlled page-fault test.
         *
         * This address (0xfffff00000000000) is canonical (PML4 index
         * 0x1E0) and is never used by Limine, MyOS's kernel, the HHDM
         * mapping, or the identity map. We still defensively verify it is
         * unmapped before deliberately faulting, so the test can never
         * corrupt live kernel memory. When the fault fires, the CPU
         * delivers vector 14, our ISR captures CR2 and the error code,
         * and the diagnostic page-fault panic is shown. */
        const uint64_t test_address = 0xfffff00000000000ULL;

        if (vm_is_mapped(test_address)) {
            kprint("pfault: address is unexpectedly mapped; aborting.\n", 0xFF0000);
        } else {
            kprint("Triggering page fault at ", 0xFFFFFF);
            kprint_hex64(test_address, 0xFFFFFF);
            kprint("\n", 0xFFFFFF);
            *(volatile uint64_t *)test_address = 0x123456789ABCDEF0ULL;
        }
    } else if (strncmp(cmd, "echo ", 5) == 0) {
        kprint(cmd + 5, 0xFFFFFF);
        kprint("\n", 0xFFFFFF);
    } else {
        kprint("Unknown command: ", 0xFF0000);
        kprint(cmd, 0xFF0000);
        kprint("\n", 0xFF0000);
    }
}

void keyboard_handler(void) {
    uint8_t scancode = inb(0x60);
    outb(0x20, 0x20);

    if (!(scancode & 0x80)) { 
        char ch = scancode_ascii[scancode];

        if (ch == '\n') {
            kputc('\n', 0xFFFFFF);
            cmd_buffer[cmd_len] = '\0';
            
            parse_command(cmd_buffer);

            cmd_len = 0;
            cmd_buffer[0] = '\0';
            
            kprint("MyOS:> ", 0x00FFFF);
        } else if (ch == '\b') {
            if (cmd_len > 0) {
                cmd_len--;
                cmd_buffer[cmd_len] = '\0';
                kbackspace();
            }
        } else if (ch != 0) {
            if (cmd_len < CMD_BUFFER_SIZE - 1) {
                cmd_buffer[cmd_len++] = ch;
                cmd_buffer[cmd_len] = '\0';
                kputc(ch, 0x00FFFF);
            }
        }
    }
}
