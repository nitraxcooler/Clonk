#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "boot/limine.h"
#include "graphics/font.h"
#include "arch/x86_64/idt.h"
#include "arch/x86_64/io.h"
#include "kernel/pmm.h"
#include "kernel/vmm.h"
#include "kernel/heap.h"
#include "kernel/blockdev.h"
#include "fs/vfs.h"
#include "fs/fat32.h"

/* Minimal serial debug helpers (COM1 0x3F8) used only for headless debugging */
static inline void serial_putc(char c) {
    outb(0x3F8, (uint8_t)c);
}
static void serial_write(const char *s) {
    while (*s) serial_putc(*s++);
}

/* Temp debug: hex dump over serial for the VFS smoke test. */
static void vfs_test_ser_hex(uint64_t v) {
    const char *d = "0123456789ABCDEF";
    char buf[19];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 15; i >= 0; --i) {
        buf[2 + (15 - i)] = d[(v >> (i * 4)) & 0xF];
    }
    buf[18] = '\0';
    serial_write(buf);
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

__attribute__((used, section(".requests")))
volatile struct limine_module_request limine_module_request = {
    .id = LIMINE_MODULE_REQUEST_ID,
    .revision = 0
};

static struct limine_framebuffer *fb;
static uint32_t cursor_x = 10;
static uint32_t cursor_y = 10;

void draw_char(char c, uint32_t x, uint32_t y, uint32_t fg_color, uint32_t bg_color) {
    if ((unsigned char)c > 127) return;

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

void kputc(char c, uint32_t fg_color) {
    if (c == '\n') {
        cursor_x = 10;
        cursor_y += 18;
    } else {
        draw_char(c, cursor_x, cursor_y, fg_color, 0x000000);
        cursor_x += 8;
        if (cursor_x >= fb->width - 10) {
            cursor_x = 10;
            cursor_y += 18;
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
        __asm__("hlt");
    }
}

void _start(void) {
    /* early serial heartbeat for headless debugging */
    serial_write("BOOT: _start\n");

    if (LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision) == false) halt();
    if (framebuffer_request.response == NULL || framebuffer_request.response->framebuffer_count < 1) halt();

    fb = framebuffer_request.response->framebuffers[0];

    /* report framebuffer address to serial for diagnostics */
    serial_write("BOOT: framebuffer OK\n");

    kclear();

    kprint("MyOS CLI Interactive Shell v0.1.0\n", 0x00FF00);
    kprint("Type 'help' for available commands.\n\n", 0xFFFFFF);

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

    serial_write("BOOT: initializing heap\n");
    heap_init();
    serial_write("BOOT: running heap self-test\n");
        heap_self_test();
    kprint("Heap initialized\n", 0x00FF00);

    serial_write("BOOT: initializing block devices\n");
    blockdev_init();
    serial_write("BOOT: block devices initialized\n");

    serial_write("BOOT: mounting filesystems\n");
    {
        static fat32_fs boot_fs; /* lives for the lifetime of the kernel */
        block_device *rootdev = blockdev_get("ramdisk0");
        if (rootdev != NULL && fat32_mount(rootdev, &boot_fs) == 0) {
            vfs_init();
            if (vfs_do_mount("/", "fat32", rootdev, &fat32_ops, &boot_fs) == 0) {
                kprint("FAT32 volume mounted on / (ramdisk0)\n", 0x00FF00);
                serial_write("BOOT: FAT32 mounted on /\n");
            } else {
                kprint("Failed to mount FAT32 volume\n", 0x00FFFF);
                serial_write("BOOT: vfs_do_mount failed\n");
            }
        } else {
            kprint("No FAT32 ramdisk found; filesystem disabled\n", 0x00FFFF);
            serial_write("BOOT: no mountable FAT32 ramdisk\n");
        }
    }

    /* Headless VFS smoke test (serial only): list / and read /HELLO.TXT. */
    {
        serial_write("VFS-TEST: ls /\n");
        for (int i = 0; i < 64; ++i) {
            char name[VFS_NAME_MAX];
            uint32_t type = 0;
            if (vfs_readdir("/", i, name, sizeof(name), &type) != 0) {
                if (i == 0) serial_write("VFS-TEST: readdir FAILED\n");
                break;
            }
            serial_write("VFS-TEST:   ");
            serial_write(name);
            serial_write(type == VFS_TYPE_DIR ? "/\n" : "\n");
        }

        vfs_file vf;
        if (vfs_open("/hello.txt", &vf) == 0 && vf.type == VFS_TYPE_FILE) {
            serial_write("VFS-TEST: opened, size=");
            vfs_test_ser_hex(vf.size);
            serial_write("\n");
            static char vbuf[64];
            uint64_t got = 0;
            int ok = (vfs_read(&vf, 0, sizeof(vbuf) - 1, vbuf, &got) == 0 &&
                      got == vf.size && got > 0);
            vbuf[got < sizeof(vbuf) ? got : sizeof(vbuf) - 1] = '\0';
            serial_write(ok ? "VFS-TEST: cat /hello.txt OK\n"
                            : "VFS-TEST: cat /hello.txt FAILED\n");
            serial_write("VFS-TEST: got=");
            vfs_test_ser_hex(got);
            serial_write("\n");
            serial_write("VFS-TEST: content: ");
            serial_write(vbuf);
            serial_write("\n");
            vfs_close(&vf);
        } else {
            serial_write("VFS-TEST: open /hello.txt FAILED\n");
        }
    }

    serial_write("BOOT: ready for CLI\n");
    kprint("MyOS:> ", 0x00FFFF);

    serial_write("BOOT: entering halt loop\n");
    halt();
}
