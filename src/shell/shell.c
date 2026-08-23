/*
 * MyOS shell.
 *
 * Commands are tokenised from the input line and routed to kernel APIs,
 * keeping the shell separate from kernel internals. Fatal misuse (such as
 * deliberate page-fault test commands) reaches kernel_panic(); ordinary
 * errors print a message instead of corrupting anything.
 */

#include "shell.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kernel/pmm.h"
#include "kernel/vmm.h"
#include "kernel/heap.h"
#include "kernel/panic.h"
#include "lib/string.h"
#include "arch/x86_64/io.h"

extern void kprint(const char *str, uint32_t fg_color);
extern void kputc(char c, uint32_t fg_color);
extern void kclear(void);

#define SHELL_MAX_TOKENS 8
#define SHELL_TOK_LEN    96
#define SHELL_TOK_WHITE  0xFFFFFF
#define SHELL_COL_OK     0x00FF00
#define SHELL_COL_HEAD   0xFFFF00
#define SHELL_COL_ERR    0xFF0000

static char shell_tokens[SHELL_MAX_TOKENS][SHELL_TOK_LEN];

static void shell_serial_putc(char c) { outb(0x3F8, (uint8_t)c); }
static void shell_serial_write(const char *s) { while (*s) shell_serial_putc(*s++); }
static void shell_serial_hex(uint64_t v) {
    const char *d = "0123456789ABCDEF";
    const char *hex = d;
    shell_serial_write("0x");
    for (int i = 15; i >= 0; --i) shell_serial_putc(hex[(v >> (i * 4)) & 0xF]);
}

void shell_print(const char *s) {
    kprint(s, SHELL_TOK_WHITE);
}

void shell_print_u64(uint64_t value) {
    if (value == 0) {
        kputc('0', SHELL_TOK_WHITE);
        return;
    }
    char buf[20];
    int n = 0;
    while (value != 0) {
        buf[n++] = (char)('0' + (value % 10U));
        value /= 10U;
    }
    for (int i = n - 1; i >= 0; --i) {
        kputc(buf[i], SHELL_TOK_WHITE);
    }
}

void shell_print_hex(uint64_t value) {
    const char *d = "0123456789ABCDEF";
    kprint("0x", SHELL_TOK_WHITE);
    for (int i = 15; i >= 0; --i) {
        kputc(d[(value >> (i * 4)) & 0xF], SHELL_TOK_WHITE);
    }
}

static int shell_tokenize(const char *line) {
    int n = 0;
    size_t i = 0;
    while (line[i] != '\0' && n < SHELL_MAX_TOKENS) {
        while (line[i] == ' ' || line[i] == '\t') ++i;
        if (line[i] == '\0') break;
        size_t len = 0;
        while (line[i] != '\0' && line[i] != ' ' && line[i] != '\t' && len < SHELL_TOK_LEN - 1) {
            shell_tokens[n][len++] = line[i++];
        }
        shell_tokens[n][len] = '\0';
        ++n;
    }
    return n;
}

static const char *tok(int i) {
    if (i < 0 || i >= SHELL_MAX_TOKENS) {
        return "";
    }
    return shell_tokens[i];
}

static void cmd_help(void) {
    static const char *lines[] = {
        "  help     - Display this menu",
        "  clear    - Clear the screen",
        "  version  - Show kernel version",
        "  echo     - Print text (echo hello)",
        "  mem      - Physical memory stats",
        "  vmem     - Virtual memory stats",
        "  heap     - Kernel heap stats",
        "  selftest - Re-run all kernel self-tests",
        "  panic    - Trigger a deliberate kernel panic",
        "  pfault   - Trigger a deliberate page fault",
        "  badalloc - Trigger out-of-memory (heap exhaustion)",
        NULL
    };
    kprint("Available commands:\n", SHELL_COL_HEAD);
    for (int i = 0; lines[i] != NULL; ++i) {
        kprint(lines[i], SHELL_TOK_WHITE);
        kprint("\n", SHELL_TOK_WHITE);
    }
}

static void cmd_mem(void) {
    if (!pmm_is_initialized()) {
        kprint("Memory manager not initialized.\n", SHELL_COL_ERR);
        return;
    }
    kprint("MyOS Memory Information\n", SHELL_COL_OK);
    kprint("Page size:       ", SHELL_TOK_WHITE); shell_print_u64(PMM_PAGE_SIZE);
    kprint(" bytes\n", SHELL_TOK_WHITE);
    kprint("Total memory:    ", SHELL_TOK_WHITE);
    shell_print_u64(pmm_get_total_memory() / (1024ULL * 1024ULL));
    kprint(" MiB\n", SHELL_TOK_WHITE);
    kprint("Usable memory:   ", SHELL_TOK_WHITE);
    shell_print_u64(pmm_get_usable_memory() / (1024ULL * 1024ULL));
    kprint(" MiB\n", SHELL_TOK_WHITE);
    kprint("Free pages:      ", SHELL_TOK_WHITE); shell_print_u64(pmm_get_free_pages());
    kprint("\n", SHELL_TOK_WHITE);
    kprint("Used pages:      ", SHELL_TOK_WHITE); shell_print_u64(pmm_get_used_pages());
    kprint("\n", SHELL_TOK_WHITE);
}

static void cmd_vmem(void) {
    if (!vm_is_initialized()) {
        kprint("Virtual memory manager not initialized.\n", SHELL_COL_ERR);
        return;
    }
    kprint("Virtual Memory\n", SHELL_COL_OK);
    kprint("Page size:       ", SHELL_TOK_WHITE); shell_print_u64(VMM_PAGE_SIZE);
    kprint(" bytes\n", SHELL_TOK_WHITE);
    kprint("PML4:            0x", SHELL_TOK_WHITE); shell_print_u64(vm_get_active_pml4());
    kprint("\n", SHELL_TOK_WHITE);
    kprint("Mapped pages:    ", SHELL_TOK_WHITE); shell_print_u64(vm_get_mapped_pages());
    kprint("\n", SHELL_TOK_WHITE);
    kprint("Page tables:     ", SHELL_TOK_WHITE); shell_print_u64(vm_get_page_table_count());
    kprint("\n", SHELL_TOK_WHITE);
}

static void cmd_heap(void) {
    if (!heap_is_initialized()) {
        kprint("Heap not initialized.\n", SHELL_COL_ERR);
        return;
    }
    heap_stats st;
    heap_get_stats(&st);
    kprint("Kernel Heap\n", SHELL_COL_OK);
    kprint("Arena bytes: ", SHELL_TOK_WHITE); shell_print_u64(st.arena_bytes); kprint("\n", SHELL_TOK_WHITE);
    kprint("Mapped bytes:", SHELL_TOK_WHITE); shell_print_u64(st.mapped_bytes); kprint("\n", SHELL_TOK_WHITE);
    kprint("Used bytes:  ", SHELL_TOK_WHITE); shell_print_u64(st.used_bytes);   kprint("\n", SHELL_TOK_WHITE);
    kprint("Free bytes:  ", SHELL_TOK_WHITE); shell_print_u64(st.free_bytes);   kprint("\n", SHELL_TOK_WHITE);
    kprint("Live allocs: ", SHELL_TOK_WHITE); shell_print_u64(st.alloc_count);  kprint("\n", SHELL_TOK_WHITE);
    kprint("Free calls:  ", SHELL_TOK_WHITE); shell_print_u64(st.free_count);   kprint("\n", SHELL_TOK_WHITE);
    kprint("Peak bytes:  ", SHELL_TOK_WHITE); shell_print_u64(st.peak_bytes);   kprint("\n", SHELL_TOK_WHITE);
}

static void cmd_selftest(void) {
    kprint("Running kernel self-tests...\n", SHELL_COL_HEAD);
    shell_serial_write("\nAPP Selftest:\n");

    pmm_self_test();
    shell_serial_write(" APP: pmm ok\n");
    vm_self_test();
    shell_serial_write(" APP: vmm ok\n");

    if (!heap_is_initialized()) {
        heap_init();
    }
    heap_self_test();
    shell_serial_write(" APP: heap ok\n");

    kprint("All self-tests passed.\n", SHELL_COL_OK);
    shell_serial_write("APP Selftest OK\n");
}

static void cmd_panic(void) {
    kernel_panic("panic command: manual kernel panic requested.");
}

static void cmd_pfault(void) {
    kprint("Triggering deliberate page fault...\n", SHELL_COL_HEAD);
    volatile uint8_t *bad = (volatile uint8_t *)0x10;
    (void)*bad;
    /* Should never reach here */
    kprint("ERROR: page fault was not triggered.\n", SHELL_COL_ERR);
}

static void cmd_badalloc(void) {
    kprint("Triggering out-of-memory...\n", SHELL_COL_HEAD);
    void *p = kmalloc(HEAP_ARENA_SIZE + 65536);
    if (p != NULL) {
        kfree(p);
        kprint("ERROR: oversized allocation unexpectedly succeeded.\n", SHELL_COL_ERR);
    }
}

void shell_handle_command(const char *cmdline) {
    if (cmdline == NULL) {
        return;
    }
    int n = shell_tokenize(cmdline);
    if (n == 0) {
        return;
    }
    const char *cmd = tok(0);

    if (strcmp(cmd, "help") == 0) {
        cmd_help();
    } else if (strcmp(cmd, "clear") == 0) {
        kclear();
    } else if (strcmp(cmd, "version") == 0) {
        kprint("MyOS v0.1.0 (x86_64 Long Mode Bare-Metal)\n", SHELL_COL_OK);
    } else if (strcmp(cmd, "echo") == 0 && n >= 2) {
        kprint(tok(1), SHELL_TOK_WHITE);
        for (int i = 2; i < n; ++i) {
            kprint(" ", SHELL_TOK_WHITE);
            kprint(tok(i), SHELL_TOK_WHITE);
        }
        kprint("\n", SHELL_TOK_WHITE);
    } else if (strcmp(cmd, "mem") == 0) {
        cmd_mem();
    } else if (strcmp(cmd, "vmem") == 0) {
        cmd_vmem();
    } else if (strcmp(cmd, "heap") == 0) {
        cmd_heap();
    } else if (strcmp(cmd, "selftest") == 0) {
        cmd_selftest();
        } else if (strcmp(cmd, "panic") == 0) {
        cmd_panic();
    } else if (strcmp(cmd, "pfault") == 0) {
        cmd_pfault();
    } else if (strcmp(cmd, "badalloc") == 0) {
        cmd_badalloc();
    } else {
        kprint("Unknown command: ", SHELL_COL_ERR);
        kprint(cmd, SHELL_COL_ERR);
        kprint("\n", SHELL_COL_ERR);
    }
}