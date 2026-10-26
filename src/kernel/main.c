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
#include "lib/mem.h"

static inline void serial_putc(char c) {
    outb(0x3F8, (uint8_t)c);
}
static void serial_write(const char *s) {
    while (*s) serial_putc(*s++);
}
static void serial_hex64(uint64_t v) {
    const char *d = "0123456789ABCDEF";
    serial_write("0x");
    for (int i = 15; i >= 0; --i) serial_putc(d[(v >> (i * 4)) & 0xF]);
}
static void serial_dec(uint64_t v) {
    char tmp[32];
    int n = 0;
    if (v == 0) {
        serial_putc('0');
        return;
    }
    while (v != 0 && n < 31) {
        tmp[n++] = (char)('0' + (v % 10U));
        v /= 10U;
    }
    while (n > 0) serial_putc(tmp[--n]);
}

static void sata_fat32_verify(void) {
    block_device *sata = blockdev_get("sata0");
    if (sata == NULL) {
        serial_write("APP: no sata0 block device\n");
        return;
    }

    serial_write("APP: sata0 sectors=");
    serial_dec(blockdev_sector_count(sata));
    serial_write(" size=");
    serial_dec(blockdev_sector_size(sata));
    serial_write("\n");

    fat32_fs *fs = (fat32_fs *)kmalloc(sizeof(fat32_fs));
    if (fs == NULL) {
        serial_write("APP: out of memory for FAT32 fs\n");
        return;
    }
    if (fat32_mount(sata, fs) != 0) {
        serial_write("APP: FAT32 mount on sata0 failed\n");
        kfree(fs);
        return;
    }
    if (vfs_do_mount("/sata", "fat32", sata, &fat32_ops, fs) != 0) {
        serial_write("APP: VFS mount of sata0 at /sata failed\n");
        kfree(fs);
        return;
    }
    serial_write("APP: FAT32 mounted at /sata from sata0\n");

    char name[VFS_NAME_MAX];
    uint32_t type;
    for (int idx = 0; vfs_readdir("/sata", idx, name, (size_t)sizeof(name), &type) == 0; ++idx) {
        serial_write("APP: /sata/");
        serial_write(name);
        serial_write(type == VFS_TYPE_DIR ? "/\n" : "\n");
    }

    vfs_file f;
    if (vfs_open("/sata/hello.txt", &f) == 0) {
        char buf[512];
        uint64_t got = 0;
        if (vfs_read(&f, 0, sizeof(buf) - 1, buf, &got) == 0 && got > 0) {
            buf[got] = '\0';
            serial_write("APP: /sata/hello.txt: ");
            serial_write(buf);
            serial_write("\n");
        }
        vfs_close(&f);
    } else {
        serial_write("APP: could not open /sata/hello.txt\n");
    }
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

static bool string_equals(const char *left, const char *right) {
    if (left == NULL || right == NULL) return false;
    while (*left != '\0' && *right != '\0' && *left == *right) {
        ++left;
        ++right;
    }
    return *left == '\0' && *right == '\0';
}

static bool path_ends_with(const char *path, const char *suffix) {
    if (path == NULL || suffix == NULL) return false;
    size_t path_length = 0;
    size_t suffix_length = 0;
    while (path[path_length] != '\0') ++path_length;
    while (suffix[suffix_length] != '\0') ++suffix_length;
    if (suffix_length > path_length) return false;
    size_t offset = path_length - suffix_length;
    for (size_t i = 0; i < suffix_length; ++i) {
        if (path[offset + i] != suffix[i]) return false;
    }
    return true;
}

static bool boot_requests_ahci_root(void) {
    if (limine_module_request.response == NULL) return false;
    struct limine_module_response *modules = limine_module_request.response;
    if (modules->modules == NULL) return false;
    for (uint64_t i = 0; i < modules->module_count; ++i) {
        struct limine_file *module = modules->modules[i];
        if (module == NULL) continue;
        if (string_equals(module->string, "clonk.storage=ahci") ||
            path_ends_with(module->path, "ahci-root.flag")) {
            serial_write("BOOT: AHCI root test marker detected\n");
            return true;
        }
    }
    serial_write("BOOT: AHCI root test marker not present\n");
    return false;
}

static bool ahci_read_self_test(block_device *dev) {
    uint8_t first_two[BLOCKDEV_SECTOR_SIZE * 2U];
    uint8_t last[BLOCKDEV_SECTOR_SIZE];
    uint8_t invalid[BLOCKDEV_SECTOR_SIZE];

    if (dev == NULL || dev->sector_size != BLOCKDEV_SECTOR_SIZE ||
        dev->sector_count < 2U) {
        serial_write("AHCI TEST: unsupported disk geometry\n");
        return false;
    }

    if (blockdev_read_sectors(dev, 0, 2, first_two) != 0 ||
        first_two[3] != 'C' || first_two[4] != 'L' ||
        first_two[5] != 'O' || first_two[6] != 'N' ||
        first_two[7] != 'K' || first_two[8] != 'F' ||
        first_two[9] != 'S' || first_two[10] != '3' ||
        first_two[510] != 0x55 || first_two[511] != 0xAA ||
        first_two[BLOCKDEV_SECTOR_SIZE] != 0x52 ||
        first_two[BLOCKDEV_SECTOR_SIZE + 1U] != 0x52 ||
        first_two[BLOCKDEV_SECTOR_SIZE + 2U] != 0x61 ||
        first_two[BLOCKDEV_SECTOR_SIZE + 3U] != 0x41) {
        serial_write("AHCI TEST: beginning/multi-sector read FAILED\n");
        return false;
    }
    serial_write("AHCI TEST: LBA 0 count 2 data verified\n");

    if (blockdev_read_sectors(dev, dev->sector_count - 1U, 1, last) != 0 ||
        last[0] != 'C' || last[1] != 'L' || last[2] != 'O' ||
        last[3] != 'N' || last[4] != 'K' || last[5] != '-' ||
        last[6] != 'A' || last[7] != 'H' || last[8] != 'C' ||
        last[9] != 'I' || last[10] != '-' || last[11] != 'E' ||
        last[12] != 'N' || last[13] != 'D') {
        serial_write("AHCI TEST: last-sector read FAILED\n");
        return false;
    }
    serial_write("AHCI TEST: final LBA data verified\n");

    if (blockdev_read_sectors(dev, dev->sector_count, 1, invalid) == 0) {
        serial_write("AHCI TEST: out-of-range read was incorrectly accepted\n");
        return false;
    }
    serial_write("AHCI TEST: out-of-range read rejected\n");
    serial_write("AHCI TEST: all read checks passed\n");
    return true;
}

static bool ahci_write_self_test(block_device *dev) {
    enum { TEST_LBA = 8, TEST_SECTORS = 2 };
    uint8_t original[BLOCKDEV_SECTOR_SIZE * TEST_SECTORS];
    uint8_t pattern[BLOCKDEV_SECTOR_SIZE * TEST_SECTORS];
    uint8_t verify[BLOCKDEV_SECTOR_SIZE * TEST_SECTORS];

    if (dev == NULL || dev->write == NULL || dev->flush == NULL ||
        dev->sector_count <= TEST_LBA + TEST_SECTORS) {
        serial_write("AHCI TEST: device is not safely writable\n");
        return false;
    }
    if (blockdev_read_sectors(dev, TEST_LBA, TEST_SECTORS, original) != 0) {
        serial_write("AHCI TEST: could not save reserved test sectors\n");
        return false;
    }
    for (size_t i = 0; i < sizeof(pattern); ++i) {
        pattern[i] = (uint8_t)(i * 37U + 11U);
    }

    bool passed = false;
    if (blockdev_write_sectors(dev, TEST_LBA, 1, pattern) != 0 ||
        blockdev_read_sectors(dev, TEST_LBA, 1, verify) != 0 ||
        memcmp(pattern, verify, BLOCKDEV_SECTOR_SIZE) != 0 ||
        blockdev_flush(dev) != 0) {
        serial_write("AHCI TEST: single-sector write/readback FAILED\n");
        goto restore;
    }
    serial_write("AHCI TEST: single-sector write/readback verified\n");

    if (blockdev_write_sectors(dev, TEST_LBA, TEST_SECTORS, pattern) != 0 ||
        blockdev_read_sectors(dev, TEST_LBA, TEST_SECTORS, verify) != 0 ||
        memcmp(pattern, verify, sizeof(pattern)) != 0 ||
        blockdev_flush(dev) != 0) {
        serial_write("AHCI TEST: multi-sector write/readback FAILED\n");
        goto restore;
    }
    serial_write("AHCI TEST: multi-sector write/readback verified\n");

    if (blockdev_write_sectors(dev, dev->sector_count - 1U, 2, pattern) == 0 ||
        blockdev_write_sectors(dev, dev->sector_count, 1, pattern) == 0) {
        serial_write("AHCI TEST: out-of-range write was accepted\n");
        goto restore;
    }
    serial_write("AHCI TEST: out-of-range writes rejected\n");
    passed = true;

restore:
    if (blockdev_write_sectors(dev, TEST_LBA, TEST_SECTORS, original) != 0 ||
        blockdev_flush(dev) != 0 ||
        blockdev_read_sectors(dev, TEST_LBA, TEST_SECTORS, verify) != 0 ||
        memcmp(original, verify, sizeof(original)) != 0) {
        serial_write("AHCI TEST: failed to restore reserved test sectors\n");
        return false;
    }
    serial_write("AHCI TEST: reserved sectors restored and flushed\n");
    return passed;
}

static bool initialize_storage(bool ahci_root) {
    if (vfs_init() != 0) {
        serial_write("STORAGE: VFS initialization failed\n");
        kprint("VFS initialization failed.\n", 0xFF0000);
        return false;
    }

    blockdev_init();
    block_device *dev = NULL;
    if (ahci_root) {
        serial_write("STORAGE: AHCI test mode requested; selecting sata0\n");
        dev = blockdev_get("sata0");
        if (dev == NULL) {
            serial_write("STORAGE: AHCI root requested but sata0 is unavailable\n");
            kprint("AHCI root requested but no SATA disk was detected.\n", 0xFF0000);
            return false;
        }
        if (dev->write == NULL || dev->flush == NULL) {
            serial_write("STORAGE: AHCI root disk lacks write/flush support\n");
            kprint("AHCI test disk is not safely writable.\n", 0xFF0000);
            return false;
        }
        if (!ahci_read_self_test(dev)) {
            serial_write("STORAGE: AHCI read validation failed; refusing fallback\n");
            return false;
        }
        if (!ahci_write_self_test(dev)) {
            serial_write("STORAGE: AHCI write validation failed; refusing fallback\n");
            return false;
        }
    } else {
        dev = blockdev_get("ramdisk0");
        if (dev == NULL) {
            for (uint64_t i = 0; i < blockdev_get_count(); ++i) {
                block_device *candidate = blockdev_get_by_index((int)i);
                if (candidate != NULL && candidate->write != NULL) {
                    dev = candidate;
                    break;
                }
            }
        }
        if (dev == NULL) {
            dev = blockdev_get_by_index(0);
        }
    }
    if (dev == NULL) {
        serial_write("STORAGE: no boot disk module found\n");
        kprint("No boot disk module found.\n", 0xFF0000);
        return false;
    }
    if (dev->write == NULL) {
        serial_write("STORAGE: root device is read-only\n");
    }
    if (fat32_mount(dev, &boot_fat32) != 0) {
        serial_write("STORAGE: FAT32 mount failed\n");
        kprint("FAT32 mount failed.\n", 0xFF0000);
        return false;
    }
    if (vfs_do_mount("/", "fat32", dev, &fat32_ops, &boot_fat32) != 0) {
        serial_write("STORAGE: root VFS mount failed\n");
        kprint("Root VFS mount failed.\n", 0xFF0000);
        return false;
    }
    if (ahci_root) {
        serial_write("STORAGE: AHCI sata0 FAT32 mounted as persistent root /\n");
    } else {
        serial_write("STORAGE: FAT32 mounted at /\n");
    }
    kprint("FAT32 mounted at /\n", 0x00FF00);
    return true;
}

void _start(void) {

    serial_write("BOOT: _start\n");

    if (LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision) == false) halt();
    if (framebuffer_request.response == NULL || framebuffer_request.response->framebuffer_count < 1) halt();

    fb = framebuffer_request.response->framebuffers[0];

    serial_write("BOOT: framebuffer OK\n");

    kclear();

    kprint("CLONK CLI Interactive Shell v0.1.1\n", 0x00FF00);
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
    bool ahci_root = boot_requests_ahci_root();
    serial_write("BOOT: initializing storage\n");
    bool storage_ready = initialize_storage(ahci_root);
    if (ahci_root && !storage_ready) {
        serial_write("BOOT: AHCI test mode halted without RAM-disk fallback\n");
        halt();
    }
    if (!ahci_root) {
        serial_write("BOOT: verifying SATA / FAT32\n");
        sata_fat32_verify();
    }

    serial_write("BOOT: launching userspace /init\n");
    usermode_run_init();

    serial_write("BOOT: ready for CLI\n");
    kprint("Clonk:> ", 0x00FFFF);

    serial_write("BOOT: entering halt loop\n");
    halt();
}
