/*
 * MyOS block-device registry.
 *
 * A minimal singly-linked list of block devices.  Each storage driver
 * calls blockdev_register() to publish a device; consumers (VFS,
 * filesystem code) look them up by name or index.
 */

#include "kernel/blockdev.h"

#include <stddef.h>

#include "drivers/storage/ahci.h"
#include "kernel/panic.h"
#include "lib/mem.h"
#include "lib/string.h"

static block_device *blockdev_list_head = NULL;
static uint64_t blockdev_count = 0;

int blockdev_register(block_device *dev) {
    if (dev == NULL || dev->name == NULL || dev->read == NULL) {
        return -1;
    }

    /* Guard against duplicate names. */
    block_device *existing = blockdev_list_head;
    while (existing != NULL) {
        if (strcmp(existing->name, dev->name) == 0) {
            return -1;
        }
        existing = existing->next;
    }

    dev->next = blockdev_list_head;
    blockdev_list_head = dev;
    blockdev_count++;
    return 0;
}

block_device *blockdev_get(const char *name) {
    if (name == NULL) {
        return NULL;
    }
    block_device *dev = blockdev_list_head;
    while (dev != NULL) {
        if (strcmp(dev->name, name) == 0) {
            return dev;
        }
        dev = dev->next;
    }
    return NULL;
}

block_device *blockdev_get_by_index(int index) {
    if (index < 0) {
        return NULL;
    }
    block_device *dev = blockdev_list_head;
    while (dev != NULL && index > 0) {
        dev = dev->next;
        --index;
    }
    return dev;
}

uint64_t blockdev_get_count(void) {
    return blockdev_count;
}

int blockdev_read_sectors(block_device *dev, uint64_t lba, uint32_t count, void *buffer) {
    if (dev == NULL || dev->read == NULL || buffer == NULL) {
        return -1;
    }
    if (count == 0) {
        return 0;
    }
    /* Overflow / out-of-range guard. */
    if (lba >= dev->sector_count || dev->sector_count - lba < count) {
        return -1;
    }
    return dev->read(dev->dev_data, lba, count, buffer);
}

int blockdev_write_sectors(block_device *dev, uint64_t lba, uint32_t count, const void *buffer) {
    if (dev == NULL || dev->write == NULL || buffer == NULL) {
        return -1;
    }
    if (count == 0) {
        return 0;
    }
    if (lba >= dev->sector_count || dev->sector_count - lba < count) {
        return -1;
    }
    return dev->write(dev->dev_data, lba, count, buffer);
}

uint32_t blockdev_sector_size(block_device *dev) {
    if (dev == NULL) {
        return 0;
    }
    return dev->sector_size;
}

uint64_t blockdev_sector_count(block_device *dev) {
    if (dev == NULL) {
        return 0;
    }
    return dev->sector_count;
}

void blockdev_init(void) {
    /* Each storage driver registers its devices here. */
    ramdisk_init();
    ahci_init();
}
