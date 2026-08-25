/*
 * MyOS shell.
 *
 * Takes whatever the user typed, splits it into words, and hands it to
 * the right kernel function. We keep the shell dumb on purpose — it
 * never pokes at kernel internals directly, it just asks for things.
 * Some commands (like the page-fault test) are deliberately dangerous
 * and end in a kernel panic. That's the point.
 */

#include "shell.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kernel/pmm.h"
#include "kernel/vmm.h"
#include "kernel/heap.h"
#include "kernel/blockdev.h"
#include "fs/vfs.h"
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
        "  blkinfo  - List block devices",
        "  ls       - List directory (ls or ls /dir)",
        "  cat      - Print a file (cat /file.txt)",
        "  touch    - Create an empty file",
        "  write    - Write text to a file (write /f.txt hello)",
        "  rm       - Delete a file",
        "  mkdir    - Create a directory",
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
    /* If we got here, the fault didn't fire. That'd be a bug. */
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

static void cmd_blkinfo(void) {
    uint64_t count = blockdev_get_count();
    kprint("Block devices: ", SHELL_COL_OK);
    shell_print_u64(count);
    kprint("\n", SHELL_TOK_WHITE);

    for (uint64_t i = 0; i < count; ++i) {
        block_device *dev = blockdev_get_by_index((int)i);
        if (dev == NULL) {
            continue;
        }
        kprint("  [", SHELL_TOK_WHITE);
        shell_print_u64(i);
        kprint("] ", SHELL_TOK_WHITE);
        kprint(dev->name, SHELL_TOK_WHITE);
        kprint("  sectors=", SHELL_TOK_WHITE);
        shell_print_u64(blockdev_sector_count(dev));
        kprint("  size=", SHELL_TOK_WHITE);
        shell_print_u64(blockdev_sector_size(dev));
        kprint("\n", SHELL_TOK_WHITE);
    }
}

static void cmd_ls(const char *path) {
    char name[VFS_NAME_MAX];
    uint32_t type;
    int idx = 0;
    int shown = 0;

    while (vfs_readdir(path, idx, name, sizeof(name), &type) == 0) {
        if (type == VFS_TYPE_DIR) {
            kprint(name, SHELL_COL_HEAD);
            kprint("/\n", SHELL_COL_HEAD);
        } else {
            kprint(name, SHELL_TOK_WHITE);
            kprint("\n", SHELL_TOK_WHITE);
        }
        shown++;
        idx++;
    }
    if (idx == 0) {
        kprint("ls: cannot read directory: ", SHELL_COL_ERR);
        kprint(path, SHELL_COL_ERR);
        kprint("\n", SHELL_COL_ERR);
    } else if (!shown) {
        kprint("(empty)\n", SHELL_TOK_WHITE);
    }
}

static void cmd_cat(const char *path) {
    vfs_file f;
    if (vfs_open(path, &f) != 0) {
        kprint("cat: no such file: ", SHELL_COL_ERR);
        kprint(path, SHELL_COL_ERR);
        kprint("\n", SHELL_COL_ERR);
        return;
    }
    if (f.type != VFS_TYPE_FILE) {
        kprint("cat: not a file: ", SHELL_COL_ERR);
        kprint(path, SHELL_COL_ERR);
        kprint("\n", SHELL_COL_ERR);
        vfs_close(&f);
        return;
    }

    static char buf[513];
    uint64_t off = 0;
    while (off < f.size) {
        uint64_t chunk = f.size - off;
        if (chunk > 512) chunk = 512;
        uint64_t got = 0;
        if (vfs_read(&f, off, chunk, buf, &got) != 0 || got == 0) {
            break;
        }
        buf[got] = '\0';
        kprint(buf, SHELL_TOK_WHITE);
        off += got;
    }
    kprint("\n", SHELL_TOK_WHITE);

    kprint("[", SHELL_COL_HEAD);
    shell_print_u64(f.size);
    kprint(" bytes]\n", SHELL_COL_HEAD);
    vfs_close(&f);
}

/* `touch` — make an empty file. */
static void cmd_touch(const char *path) {
    if (vfs_create(path) != 0) {
        kprint("touch: cannot create: ", SHELL_COL_ERR);
        kprint(path, SHELL_COL_ERR);
        kprint("\n", SHELL_COL_ERR);
        return;
    }
    kprint("created ", SHELL_COL_OK);
    kprint(path, SHELL_TOK_WHITE);
    kprint("\n", SHELL_TOK_WHITE);
}

/* `write` — write a string into a file, starting at the beginning. */
static void cmd_write(const char *path, const char *text) {
    vfs_file f;
    if (vfs_open(path, &f) != 0) {
        /* If it doesn't exist yet, just create it like a shell would. */
        if (vfs_create(path) != 0 || vfs_open(path, &f) != 0) {
            kprint("write: cannot open: ", SHELL_COL_ERR);
            kprint(path, SHELL_COL_ERR);
            kprint("\n", SHELL_COL_ERR);
            return;
        }
    }
    if (f.type != VFS_TYPE_FILE) {
        kprint("write: not a file: ", SHELL_COL_ERR);
        kprint(path, SHELL_COL_ERR);
        kprint("\n", SHELL_COL_ERR);
        vfs_close(&f);
        return;
    }
    uint64_t written = 0;
    uint64_t len = strlen(text);
    int rc = vfs_write(&f, 0, len, text, &written);
    vfs_close(&f);
    if (rc != 0 || written != len) {
        kprint("write: failed (wrote ", SHELL_COL_ERR);
        shell_print_u64(written);
        kprint("/", SHELL_COL_ERR);
        shell_print_u64(len);
        kprint(" bytes)\n", SHELL_COL_ERR);
        return;
    }
    kprint("wrote ", SHELL_COL_OK);
    shell_print_u64(written);
    kprint(" bytes to ", SHELL_TOK_WHITE);
    kprint(path, SHELL_TOK_WHITE);
    kprint("\n", SHELL_TOK_WHITE);
}

/* `rm` — delete a file. */
static void cmd_rm(const char *path) {
    if (vfs_unlink(path) != 0) {
        kprint("rm: cannot remove: ", SHELL_COL_ERR);
        kprint(path, SHELL_COL_ERR);
        kprint("\n", SHELL_COL_ERR);
        return;
    }
    kprint("removed ", SHELL_COL_OK);
    kprint(path, SHELL_TOK_WHITE);
    kprint("\n", SHELL_TOK_WHITE);
}

/* `mkdir` — make a new directory. */
static void cmd_mkdir(const char *path) {
    if (vfs_mkdir(path) != 0) {
        kprint("mkdir: cannot create: ", SHELL_COL_ERR);
        kprint(path, SHELL_COL_ERR);
        kprint("\n", SHELL_COL_ERR);
        return;
    }
    kprint("created dir ", SHELL_COL_OK);
    kprint(path, SHELL_TOK_WHITE);
    kprint("\n", SHELL_TOK_WHITE);
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
        kprint("MyOS v0.1.1 (x86_64 Long Mode Bare-Metal)\n", SHELL_COL_OK);
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
    } else if (strcmp(cmd, "blkinfo") == 0) {
        cmd_blkinfo();
    } else if (strcmp(cmd, "ls") == 0) {
        cmd_ls(n >= 2 ? tok(1) : "/");
    } else if (strcmp(cmd, "cat") == 0 && n >= 2) {
        cmd_cat(tok(1));
    } else {
        kprint("Unknown command: ", SHELL_COL_ERR);
        kprint(cmd, SHELL_COL_ERR);
        kprint("\n", SHELL_COL_ERR);
    }
}