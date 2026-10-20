#ifndef FAT32_H
#define FAT32_H

#include <stdint.h>

#include "fs/vfs.h"
#include "kernel/blockdev.h"

typedef struct fat32_fs {
    block_device *dev;
    uint16_t  bytes_per_sector;
    uint8_t   sectors_per_cluster;
    uint16_t  reserved_sectors;
    uint8_t   fat_count;
    uint32_t  fat_size_sectors;
    uint32_t  root_cluster;
    uint32_t  total_sectors;
    uint32_t  data_start_sector;
    uint32_t  cluster_bytes;
    uint8_t   valid;
    uint32_t  next_cluster;
} fat32_fs;

int fat32_mount(block_device *dev, fat32_fs *fs);

extern struct vfs_fs_ops fat32_ops;

#endif
