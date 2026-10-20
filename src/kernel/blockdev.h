#ifndef BLOCKDEV_H
#define BLOCKDEV_H

#include <stdint.h>

#define BLOCKDEV_SECTOR_SIZE 512

typedef int (*blockdev_read_fn)(void *dev_data, uint64_t lba, uint32_t count, void *buffer);
typedef int (*blockdev_write_fn)(void *dev_data, uint64_t lba, uint32_t count, const void *buffer);
typedef int (*blockdev_flush_fn)(void *dev_data);

typedef struct block_device {
    const char *name;
    uint64_t sector_count;
    uint32_t sector_size;
    void     *dev_data;
    blockdev_read_fn  read;
    blockdev_write_fn write;
    blockdev_flush_fn flush;
    struct block_device *next;
} block_device;

int         blockdev_register(block_device *dev);
block_device *blockdev_get(const char *name);
block_device *blockdev_get_by_index(int index);
uint64_t    blockdev_get_count(void);

int         blockdev_read_sectors(block_device *dev, uint64_t lba, uint32_t count, void *buffer);
int         blockdev_write_sectors(block_device *dev, uint64_t lba, uint32_t count, const void *buffer);
int         blockdev_flush(block_device *dev);
uint32_t    blockdev_sector_size(block_device *dev);
uint64_t    blockdev_sector_count(block_device *dev);

void        blockdev_init(void);

void        ramdisk_init(void);
void        ahci_init(void);

#endif
