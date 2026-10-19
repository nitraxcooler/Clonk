#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "boot/limine.h"
#include "graphics/font.h"
#include "arch/x86_64/idt.h"
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/io.h"
#include "kernel/pmm.h"
#include "kernel/vmm.h"
#include "kernel/usermode.h"
#include "kernel/elf.h"
#include "kernel/heap.h"
#include "kernel/blockdev.h"
#include "fs/vfs.h"
#include "fs/fat32.h"
#include "drivers/keyboard/keyboard.h"

/* These helpers write to the serial port (COM1) so we can see what's
 * happening even when there's no screen attached. */
static inline void serial_putc(char c) {
    outb(0x3F8, (uint8_t)c);
}
static void serial_write(const char *s) {
    while (*s) serial_putc(*s++);
}

__attribute__((used, section(".requests")))
volatile uint64_t limine_base_revision[3] = LIMINE_BASE_REVISION(0);

__attribute__((used, section(".requests")))
volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".requests")))
volatile struct limine_memmap_request limine_memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".requests")))
volatile struct limine_hhdm_request limine_hhdm_request = {
    .id = LIMINE_HHDM_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".requests")))
volatile struct limine_executable_address_request limine_executable_address_request = {
    .id = LIMINE_EXECUTABLE_ADDRESS_REQUEST_ID,
    .revision = 0
};

static struct limine_framebuffer *fb;
static uint32_t cursor_x = 10;
static uint32_t cursor_y = 10;
static fat32_fs boot_fat32;

void draw_char(char c, uint32_t x, uint32_t y, uint32_t fg_color, uint32_t bg_color) {
    if ((unsigned char)c > 127) return;
    if (x > fb->width || fb->width - x < 8 ||
        y > fb->height || fb->height - y < 16) return;

    volatile uint32_t *fb_ptr = (volatile uint32_t *)fb->address;
    uint32_t pitch_pixels = fb->pitch / 4;

    for (int row = 0; row < 16; row++) {
        uint8_t glyph_row = font8x16[(unsigned char)c][row];
        for (int col = 0; col < 8; col++) {
            uint32_t pixel_color = (glyph_row & (1 << (7 - col))) ? fg_color : bg_color;
            fb_ptr[(y + row) * pitch_pixels + (x + col)] = pixel_color;
        }
    }
}

static void scroll_console(void) {
    uint32_t pitch_pixels = fb->pitch / 4;
    volatile uint32_t *pixels = (volatile uint32_t *)fb->address;
    if (fb->height > 18) {
        for (uint32_t y = 0; y + 18 < fb->height; ++y) {
            for (uint32_t x = 0; x < pitch_pixels; ++x) {
                pixels[y * pitch_pixels + x] = pixels[(y + 18) * pitch_pixels + x];
            }
        }
        for (uint32_t y = fb->height - 18; y < fb->height; ++y) {
            for (uint32_t x = 0; x < pitch_pixels; ++x) {
                pixels[y * pitch_pixels + x] = 0;
            }
        }
    } else {
        for (uint32_t i = 0; i < pitch_pixels * fb->height; ++i) {
            pixels[i] = 0;
        }
        cursor_y = 0;
        return;
    }
    cursor_y = cursor_y >= 18 ? cursor_y - 18 : 0;
}

static void keep_cursor_visible(void) {
    if (fb->height <= 16) {
        cursor_y = 0;
        return;
    }
    while (cursor_y + 16 > fb->height) {
        scroll_console();
    }
}

void kputc(char c, uint32_t fg_color) {
    if (c == '\n') {
        cursor_x = 10;
        cursor_y += 18;
        keep_cursor_visible();
    } else {
        draw_char(c, cursor_x, cursor_y, fg_color, 0x000000);
        cursor_x += 8;
        if (cursor_x >= fb->width - 10) {
            cursor_x = 10;
            cursor_y += 18;
            keep_cursor_visible();
        }
    }
}

void kprint(const char *str, uint32_t fg_color) {
    while (*str) {
        kputc(*str, fg_color);
        str++;
    }
}

void kbackspace(void) {
    if (cursor_x >= 18) {
        cursor_x -= 8;
        draw_char(' ', cursor_x, cursor_y, 0x000000, 0x000000);
    }
}

void kclear(void) {
    volatile uint32_t *fb_ptr = (volatile uint32_t *)fb->address;
    for (size_t i = 0; i < (fb->pitch / 4) * fb->height; i++) {
        fb_ptr[i] = 0x000000;
    }
    cursor_x = 10;
    cursor_y = 10;
}

static void halt(void) {
    for (;;) {
        keyboard_process_pending();
        keyboard_wait_for_event();
    }
}

static void initialize_storage(void) {
    if (vfs_init() != 0) {
        serial_write("STORAGE: VFS initialization failed\n");
        kprint("VFS initialization failed.\n", 0xFF0000);
        return;
    }

    blockdev_init();
    block_device *dev = blockdev_get_by_index(0);
    if (dev == NULL) {
        serial_write("STORAGE: no boot disk module found\n");
        kprint("No boot disk module found.\n", 0xFF0000);
        return;
    }
    if (fat32_mount(dev, &boot_fat32) != 0) {
        serial_write("STORAGE: FAT32 mount failed\n");
        kprint("FAT32 mount failed.\n", 0xFF0000);
        return;
    }
    if (vfs_do_mount("/", "fat32", dev, &fat32_ops, &boot_fat32) != 0) {
        serial_write("STORAGE: root VFS mount failed\n");
        kprint("Root VFS mount failed.\n", 0xFF0000);
        return;
    }
    serial_write("STORAGE: FAT32 mounted at /\n");
    kprint("FAT32 mounted at /\n", 0x00FF00);
}

void _start(void) {
    /* Let the serial log know we made it into the kernel. */
    serial_write("BOOT: _start\n");

    if (LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision) == false) halt();
    if (framebuffer_request.response == NULL || framebuffer_request.response->framebuffer_count < 1) halt();

    fb = framebuffer_request.response->framebuffers[0];

    /* Tell the serial log that the framebuffer is ready to use. */
    serial_write("BOOT: framebuffer OK\n");

    kclear();

    kprint("MyOS CLI Interactive Shell v0.1.1\n", 0x00FF00);
    kprint("Type 'help' for available commands.\n\n", 0xFFFFFF);

    serial_write("BOOT: initializing GDT and TSS\n");
    arch_cpu_init();
    serial_write("BOOT: initializing IDT\n");
    idt_init();
    serial_write("BOOT: initializing PMM\n");
    pmm_init();
    serial_write("BOOT: initializing VMM\n");
    vm_init();
    serial_write("BOOT: vm_init returned\n");
    kprint("PMM initialized\n", 0x00FF00);
    kprint("Paging initialized\n", 0x00FF00);
    kprint("pages:", 0xFFFFFF);
    char tmp[32];
    uint64_t tp = pmm_get_total_pages();
    uint64_t fp = pmm_get_free_pages();
    uint64_t up = pmm_get_used_pages();
    for (int i = 0; i < 32; ++i) tmp[i] = 0;
    uint64_t v = tp;
    int n = 0;
    do {
        tmp[n++] = (char)('0' + (v % 10U));
        v /= 10U;
    } while (v != 0U);
    for (int i = n - 1; i >= 0; --i) kputc(tmp[i], 0xFFFFFF);
    kprint(" free:", 0xFFFFFF);
    v = fp; n = 0;
    do {
        tmp[n++] = (char)('0' + (v % 10U));
        v /= 10U;
    } while (v != 0U);
    for (int i = n - 1; i >= 0; --i) kputc(tmp[i], 0xFFFFFF);
    kprint(" used:", 0xFFFFFF);
    v = up; n = 0;
    do {
        tmp[n++] = (char)('0' + (v % 10U));
        v /= 10U;
    } while (v != 0U);
    for (int i = n - 1; i >= 0; --i) kputc(tmp[i], 0xFFFFFF);
    kprint("\n", 0xFFFFFF);
    serial_write("BOOT: running PMM self-test\n");
    pmm_self_test();
    serial_write("BOOT: running VM self-test\n");
    vm_self_test();
    serial_write("BOOT: running ELF self-test\n");
    elf_self_test();
    serial_write("ELF: self-test passed\n");

    serial_write("BOOT: initializing heap\n");
    heap_init();
    serial_write("BOOT: initializing storage\n");
    initialize_storage();

    serial_write("BOOT: launching userspace /init\n");
    usermode_run_init();

    serial_write("BOOT: ready for CLI\n");
    kprint("MyOS:> ", 0x00FFFF);

    serial_write("BOOT: entering halt loop\n");
    halt();
}
