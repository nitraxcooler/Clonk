/*
 * MyOS RAM-disk driver.
 *
 * A RAM disk is just a chunk of memory loaded by Limine as a module,
 * presented as a block device. Reads and writes are plain memcpy calls —
 * no hardware access is involved.
 */

#include "kernel/blockdev.h"

#include <stddef.h>
#include <stdint.h>

#include "boot/limine.h"
#include "kernel/heap.h"
#include "lib/mem.h"
#include "arch/x86_64/io.h"

extern volatile struct limine_module_request limine_module_request;

/* Serial helpers so RAM-disk init shows up in the boot log. */
static inline void rd_putc(char c) { outb(0x3F8, (uint8_t)c); }
static void rd_write(const char *s) { while (*s) rd_putc(*s++); }
static void rd_hex64(uint64_t v) {
    const char *d = "0123456789ABCDEF";
    rd_write("0x");
    for (int i = 15; i >= 0; --i) rd_putc(d[(v >> (i * 4)) & 0xF]);
}

/* Each RAM disk keeps its buffer and size here. */
struct ramdisk_data {
    uint8_t *buffer;
    uint64_t  byte_size;
};

static int ramdisk_read(void *dev_data, uint64_t lba, uint32_t count, void *buffer) {
    struct ramdisk_data *rd = (struct ramdisk_data *)dev_data;
    uint64_t offset = lba * BLOCKDEV_SECTOR_SIZE;
    uint64_t length = count * BLOCKDEV_SECTOR_SIZE;

    if (offset + length > rd->byte_size) {
        return -1;
    }
    memcpy(buffer, rd->buffer + offset, length);
    return 0;
}

static int ramdisk_write(void *dev_data, uint64_t lba, uint32_t count, const void *buffer) {
    struct ramdisk_data *rd = (struct ramdisk_data *)dev_data;
    uint64_t offset = lba * BLOCKDEV_SECTOR_SIZE;
    uint64_t length = count * BLOCKDEV_SECTOR_SIZE;

    if (offset + length > rd->byte_size) {
        return -1;
    }
    memcpy(rd->buffer + offset, buffer, length);
    return 0;
}

static const char *ramdisk_names[] = {
    "ramdisk0", "ramdisk1", "ramdisk2", "ramdisk3",
    "ramdisk4", "ramdisk5", "ramdisk6", "ramdisk7"
};

/* Scan Limine modules and create a RAM disk for each one. */
void ramdisk_init(void) {
    if (limine_module_request.response == NULL) {
        rd_write("RAMDISK: no module response\n");
        return;
    }

    struct limine_module_response *resp = limine_module_request.response;
    uint64_t count = resp->module_count;
    rd_write("RAMDISK: module count "); rd_hex64(count); rd_write("\n");

    if (count > sizeof(ramdisk_names) / sizeof(ramdisk_names[0])) {
        count = sizeof(ramdisk_names) / sizeof(ramdisk_names[0]);
    }

    for (uint64_t i = 0; i < count; ++i) {
        struct limine_file *file = resp->modules[i];
        if (file == NULL || file->size == 0) {
            rd_write("RAMDISK: module NULL or empty\n");
            continue;
        }

        rd_write("RAMDISK: module addr "); rd_hex64((uint64_t)file->address);
        rd_write(" size "); rd_hex64(file->size); rd_write("\n");

        block_device *dev = kmalloc(sizeof(block_device));
        struct ramdisk_data *rd = kmalloc(sizeof(struct ramdisk_data));

        if (dev == NULL || rd == NULL) {
            /* No memory for bookkeeping — skip this module. */
            continue;
        }

        rd->buffer    = (uint8_t *)file->address;
        rd->byte_size = file->size;

        dev->name        = ramdisk_names[i];
        dev->sector_count = (file->size + BLOCKDEV_SECTOR_SIZE - 1) / BLOCKDEV_SECTOR_SIZE;
        dev->sector_size  = BLOCKDEV_SECTOR_SIZE;
        dev->dev_data     = rd;
        dev->read         = ramdisk_read;
        dev->write        = ramdisk_write;
        dev->next         = NULL;

        /* Registration fails only on a duplicate name, which can't
         * happen here; if it does, we just move on. */
        blockdev_register(dev);
    }
}
