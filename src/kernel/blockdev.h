#ifndef BLOCKDEV_H
#define BLOCKDEV_H

#include <stdint.h>

/*
 * MyOS block-device abstraction.
 *
 * A block_device represents an addressable storage medium that reads and
 * writes fixed-size blocks (sectors).  Storage drivers (RAM disk, ATA,
 * NVMe, …) register instances; the VFS and filesystem code talk to
 * devices only through this interface so that higher layers never depend
 * on the underlying hardware.
 *
 * Sector size is 512 bytes — the historical and still-universal size
 * for disk sectors.  Drivers must guarantee that every 512-byte aligned
 * LBA reads correctly.
 */

#define BLOCKDEV_SECTOR_SIZE 512

/* Return 0 on success, non-zero on error (e.g. out-of-range LBA). */
typedef int (*blockdev_read_fn)(void *dev_data, uint64_t lba, uint32_t count, void *buffer);
typedef int (*blockdev_write_fn)(void *dev_data, uint64_t lba, uint32_t count, const void *buffer);

typedef struct block_device {
    const char *name;       /* human-readable label, e.g. "ramdisk0"    */
    uint64_t sector_count;  /* total number of sectors                  */
    uint32_t sector_size;   /* bytes per sector (usually 512)           */
    void     *dev_data;     /* opaque driver context                    */
    blockdev_read_fn  read; /* mandatory                                */
    blockdev_write_fn write; /* may be NULL for read-only devices       */
    struct block_device *next;
} block_device;

/* --- registry --- */
int         blockdev_register(block_device *dev);
block_device *blockdev_get(const char *name);
block_device *blockdev_get_by_index(int index);
uint64_t    blockdev_get_count(void);

/* --- convenience wrappers --- */
int         blockdev_read_sectors(block_device *dev, uint64_t lba, uint32_t count, void *buffer);
int         blockdev_write_sectors(block_device *dev, uint64_t lba, uint32_t count, const void *buffer);
uint32_t    blockdev_sector_size(block_device *dev);
uint64_t    blockdev_sector_count(block_device *dev);

/* --- initialisation (scans Limine modules for RAM disks, PCI for SATA) --- */
void        blockdev_init(void);

/* Driver entry points — called by blockdev_init(). */
void        ramdisk_init(void);
void        ahci_init(void);

#endif