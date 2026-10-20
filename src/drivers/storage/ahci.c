
#include "drivers/storage/ahci.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arch/x86_64/io.h"
#include "drivers/pci/pci.h"
#include "kernel/blockdev.h"
#include "kernel/heap.h"
#include "kernel/pmm.h"
#include "kernel/vmm.h"
#include "lib/mem.h"

#define AHCI_HBA_CAP    0x00
#define AHCI_HBA_GHC    0x04
#define AHCI_HBA_IS     0x08
#define AHCI_HBA_PI     0x0C
#define AHCI_HBA_VS     0x10

#define AHCI_HBA_PORTS  0x100
#define AHCI_PORT_STRIDE 0x80

#define AHCI_GHC_HR     (1U << 0)
#define AHCI_GHC_IE     (1U << 1)
#define AHCI_GHC_AE     (1U << 31)
#define AHCI_CAP_S64A   (1U << 31)

#define AHCI_P_CLB      0x00
#define AHCI_P_CLBU     0x04
#define AHCI_P_FB       0x08
#define AHCI_P_FBU      0x0C
#define AHCI_P_IS       0x10
#define AHCI_P_IE       0x14
#define AHCI_P_CMD      0x18
#define AHCI_P_TFD      0x20
#define AHCI_P_SIG      0x24
#define AHCI_P_SSTS     0x28
#define AHCI_P_SCTL     0x2C
#define AHCI_P_SERR     0x30
#define AHCI_P_CI       0x38

#define AHCI_PCMD_ST       (1U << 0)
#define AHCI_PCMD_SPIN_UP  (1U << 1)
#define AHCI_PCMD_POWER_ON (1U << 2)
#define AHCI_PCMD_FIS_RX   (1U << 4)
#define AHCI_PCMD_FR       (1U << 14)
#define AHCI_PCMD_CR       (1U << 15)

#define AHCI_PIS_DHRS (1U << 0)
#define AHCI_PIS_PSS  (1U << 1)
#define AHCI_PIS_UFS  (1U << 4)
#define AHCI_PIS_DPS  (1U << 5)
#define AHCI_PIS_OFS  (1U << 24)
#define AHCI_PIS_INFS (1U << 26)
#define AHCI_PIS_IFS  (1U << 27)
#define AHCI_PIS_HBDS (1U << 28)
#define AHCI_PIS_HBFS (1U << 29)
#define AHCI_PIS_TFES (1U << 30)
#define AHCI_TFD_ERR  (1U << 0)
#define AHCI_TFD_DRQ  (1U << 3)
#define AHCI_TFD_BSY  (1U << 7)

#define AHCI_PIS_ERROR (AHCI_PIS_UFS | AHCI_PIS_DPS | AHCI_PIS_OFS | \
                        AHCI_PIS_INFS | AHCI_PIS_IFS | AHCI_PIS_HBDS | \
                        AHCI_PIS_HBFS | AHCI_PIS_TFES)

#define ATA_CMD_READ_DMA_EXT    0x25
#define ATA_CMD_WRITE_DMA_EXT   0x35
#define ATA_CMD_IDENTIFY        0xEC
#define ATA_CMD_FLUSH_CACHE     0xE7
#define ATA_CMD_FLUSH_CACHE_EXT 0xEA

#define AHCI_SIG_DISK   0x00000101U
#define AHCI_SIG_CDROM  0xEB140101U
#define AHCI_SSTS_DET_MASK 0xFU
#define AHCI_SSTS_DET_DEV  3U

struct ahci_cmd_hdr {
    uint16_t opts;
    uint16_t prdtl;
    uint32_t prdbc;
    uint64_t ctba;
    uint32_t reserved[4];
} __attribute__((packed));

struct ahci_prd {
    uint64_t dba;
    uint32_t reserved;
    uint32_t flags;
} __attribute__((packed));

#define AHCI_CMD_TABLE_PRDT_OFFSET 0x80

#define AHCI_MAX_PORTS        32
#define AHCI_MAX_DISKS        8
#define AHCI_CMD_SLOT         0
#define AHCI_DMA_SECTORS      8
#define AHCI_TIMEOUT_LOOPS    10000000ULL

struct ahci_dev {
    uint32_t *hba;
    unsigned  port;
    uint64_t  sector_count;
    uint32_t  sector_size;
    bool      flush_supported;
    bool      flush_ext;

    uint64_t  clb_phys;
    uint64_t  fb_phys;
    uint64_t  ctba_phys;
    uint64_t  dma_phys;
    void     *dma_virt;

    char      name[16];
};

#define AHCI_MMIO_VIRT 0xffffffffff000000ULL

static inline void ahci_putc(char c) { outb(0x3F8, (uint8_t)c); }
static void ahci_serial(const char *s) { while (*s) ahci_putc(*s++); }
static void ahci_serial_hex64(uint64_t v) {
    const char *d = "0123456789ABCDEF";
    ahci_serial("0x");
    for (int i = 15; i >= 0; --i) ahci_putc(d[(v >> (i * 4)) & 0xF]);
}
static void ahci_serial_hex32(uint32_t v) {
    const char *d = "0123456789ABCDEF";
    ahci_serial("0x");
    for (int i = 7; i >= 0; --i) ahci_putc(d[(v >> (i * 4)) & 0xF]);
}

static inline uint32_t ahci_read32(uint32_t *base, uint32_t offset) {
    return *(volatile uint32_t *)((uintptr_t)base + offset);
}

static inline void ahci_write32(uint32_t *base, uint32_t offset, uint32_t value) {
    *(volatile uint32_t *)((uintptr_t)base + offset) = value;
}

static inline uint32_t *ahci_port_base(uint32_t *hba, unsigned port) {
    return (uint32_t *)((uintptr_t)hba + AHCI_HBA_PORTS + port * AHCI_PORT_STRIDE);
}

static void ahci_fill_command(struct ahci_dev *d, uint8_t command,
                              uint64_t lba, uint16_t sectors, uint32_t bytes) {
    struct ahci_cmd_hdr *hdr =
        (struct ahci_cmd_hdr *)(uintptr_t)vm_phys_to_virt(d->clb_phys);
    uint8_t *table = (uint8_t *)(uintptr_t)vm_phys_to_virt(d->ctba_phys);
    uint8_t *fis = table;
    struct ahci_prd *prd =
        (struct ahci_prd *)(table + AHCI_CMD_TABLE_PRDT_OFFSET);

    memset(hdr, 0, sizeof(*hdr));
    memset(fis, 0, AHCI_CMD_TABLE_PRDT_OFFSET);

    hdr->opts  = 5;
    hdr->prdtl = bytes == 0U ? 0U : 1U;
    hdr->prdbc = 0;
    hdr->ctba  = d->ctba_phys;

    fis[0]  = 0x27;
    fis[1]  = 0x80;
    fis[2]  = command;
    fis[3]  = 0;
    fis[4]  = (uint8_t)(lba & 0xFF);
    fis[5]  = (uint8_t)((lba >> 8) & 0xFF);
    fis[6]  = (uint8_t)((lba >> 16) & 0xFF);
    fis[7]  = (command == ATA_CMD_READ_DMA_EXT ||
               command == ATA_CMD_WRITE_DMA_EXT) ? 0x40 : 0;
    fis[8]  = (uint8_t)((lba >> 24) & 0xFF);
    fis[9]  = (uint8_t)((lba >> 32) & 0xFF);
    fis[10] = (uint8_t)((lba >> 40) & 0xFF);
    fis[11] = 0;
    fis[12] = (uint8_t)(sectors & 0xFF);
    fis[13] = (uint8_t)((sectors >> 8) & 0xFF);

    if (bytes != 0U) {
        prd[0].dba   = d->dma_phys;
        prd[0].flags = (bytes - 1U) & 0x3FFFFF;
    }
}

static int ahci_issue_slot(struct ahci_dev *d) {
    uint32_t *port = ahci_port_base(d->hba, d->port);
    for (uint64_t i = 0; i < AHCI_TIMEOUT_LOOPS; ++i) {
        uint32_t tfd = ahci_read32(port, AHCI_P_TFD);
        uint32_t ci = ahci_read32(port, AHCI_P_CI);
        if ((tfd & (AHCI_TFD_BSY | AHCI_TFD_DRQ)) == 0U &&
            (ci & (1U << AHCI_CMD_SLOT)) == 0U) {
            if ((tfd & AHCI_TFD_ERR) != 0U) return -1;
            ahci_write32(port, AHCI_P_IS, 0xFFFFFFFF);
            __asm__ volatile ("" : : : "memory");
            ahci_write32(port, AHCI_P_CI, 1U << AHCI_CMD_SLOT);
            return 0;
        }
        io_wait();
    }
    ahci_serial("AHCI: port ");
    ahci_serial_hex32(d->port);
    ahci_serial(" command slot unavailable\n");
    return -1;
}

static int ahci_wait_slot(struct ahci_dev *d) {
    uint32_t *port = ahci_port_base(d->hba, d->port);

    for (uint64_t i = 0; i < AHCI_TIMEOUT_LOOPS; ++i) {
        uint32_t is = ahci_read32(port, AHCI_P_IS);
        if (is & AHCI_PIS_ERROR) {
            ahci_serial("AHCI: port ");
            ahci_serial_hex32(d->port);
            ahci_serial(" PxIS error: ");
            ahci_serial_hex32(is);
            ahci_serial("\n");
            ahci_write32(port, AHCI_P_IS, is);
            return -1;
        }

        if ((ahci_read32(port, AHCI_P_CI) & (1U << AHCI_CMD_SLOT)) == 0) {

            uint32_t tfd = ahci_read32(port, AHCI_P_TFD);
            if ((tfd & (AHCI_TFD_BSY | AHCI_TFD_DRQ)) == 0) {
                if ((tfd & AHCI_TFD_ERR) != 0) {
                    ahci_serial("AHCI: port ");
                    ahci_serial_hex32(d->port);
                    ahci_serial(" task-file error: ");
                    ahci_serial_hex32(tfd);
                    ahci_serial("\n");
                    ahci_write32(port, AHCI_P_IS, 0xFFFFFFFF);
                    return -1;
                }
                ahci_write32(port, AHCI_P_IS, ahci_read32(port, AHCI_P_IS));
                __asm__ volatile ("mfence" : : : "memory");
                return 0;
            }
        }
        io_wait();
    }

    ahci_serial("AHCI: port ");
    ahci_serial_hex32(d->port);
    ahci_serial(" command timeout\n");
    return -1;
}

static int ahci_read_sectors(void *dev_data, uint64_t lba, uint32_t count,
                             void *buffer) {
    struct ahci_dev *d = (struct ahci_dev *)dev_data;
    uint8_t *out = (uint8_t *)buffer;

    if (d == NULL || buffer == NULL || d->sector_size != BLOCKDEV_SECTOR_SIZE ||
        lba >= d->sector_count || (uint64_t)count > d->sector_count - lba ||
        lba >= (1ULL << 48) || (uint64_t)count > (1ULL << 48) - lba) {
        return -1;
    }

    while (count > 0) {
        uint32_t chunk = count > AHCI_DMA_SECTORS ? AHCI_DMA_SECTORS : count;

        ahci_fill_command(d, ATA_CMD_READ_DMA_EXT, lba, (uint16_t)chunk,
                          chunk * BLOCKDEV_SECTOR_SIZE);
        if (ahci_issue_slot(d) != 0) return -1;
        if (ahci_wait_slot(d) != 0) {
            return -1;
        }

        memcpy(out, d->dma_virt, chunk * BLOCKDEV_SECTOR_SIZE);
        lba += chunk;
        count -= chunk;
        out += chunk * BLOCKDEV_SECTOR_SIZE;
    }
    return 0;
}

static int ahci_flush_device(void *dev_data) {
    struct ahci_dev *d = (struct ahci_dev *)dev_data;
    if (d == NULL || !d->flush_supported) return -1;
    uint8_t command = d->flush_ext ? ATA_CMD_FLUSH_CACHE_EXT :
                                     ATA_CMD_FLUSH_CACHE;
    ahci_fill_command(d, command, 0, 0, 0);
    if (ahci_issue_slot(d) != 0) return -1;
    return ahci_wait_slot(d);
}

static int ahci_write_sectors(void *dev_data, uint64_t lba, uint32_t count,
                              const void *buffer) {
    struct ahci_dev *d = (struct ahci_dev *)dev_data;
    const uint8_t *input = (const uint8_t *)buffer;

    if (d == NULL || buffer == NULL || !d->flush_supported ||
        d->sector_size != BLOCKDEV_SECTOR_SIZE ||
        lba >= d->sector_count || (uint64_t)count > d->sector_count - lba ||
        lba >= (1ULL << 48) || (uint64_t)count > (1ULL << 48) - lba) {
        return -1;
    }

    while (count > 0U) {
        uint32_t chunk = count > AHCI_DMA_SECTORS ? AHCI_DMA_SECTORS : count;
        uint32_t bytes = chunk * BLOCKDEV_SECTOR_SIZE;
        memcpy(d->dma_virt, input, bytes);
        ahci_fill_command(d, ATA_CMD_WRITE_DMA_EXT, lba, (uint16_t)chunk, bytes);
        if (ahci_issue_slot(d) != 0) return -1;
        if (ahci_wait_slot(d) != 0) return -1;
        lba += chunk;
        count -= chunk;
        input += bytes;
    }

    return ahci_flush_device(d);
}

static int ahci_identify(struct ahci_dev *d) {
    ahci_fill_command(d, ATA_CMD_IDENTIFY, 0, 0, 512);
    if (ahci_issue_slot(d) != 0) return -1;
    if (ahci_wait_slot(d) != 0) {
        return -1;
    }

    const uint16_t *id = (const uint16_t *)d->dma_virt;
    if (id[0] == 0 || id[0] == 0xFFFF) {
        ahci_serial("AHCI: IDENTIFY returned no drive signature\n");
        return -1;
    }

    if ((id[83] & 0xC000U) != 0x4000U || (id[83] & (1U << 10)) == 0) {
        ahci_serial("AHCI: device does not report valid LBA48 support\n");
        return -1;
    }

    uint64_t lba48 = (uint64_t)id[100] | ((uint64_t)id[101] << 16) |
                     ((uint64_t)id[102] << 32) | ((uint64_t)id[103] << 48);
    d->sector_count = lba48;
    if (d->sector_count == 0) {
        ahci_serial("AHCI: IDENTIFY reports zero sectors\n");
        return -1;
    }

    d->sector_size = BLOCKDEV_SECTOR_SIZE;
    if ((id[106] & 0xC000U) == 0x4000U && (id[106] & (1U << 12)) != 0) {
        uint32_t logical_words = (uint32_t)id[117] | ((uint32_t)id[118] << 16);
        if (logical_words == 0U || logical_words > UINT32_MAX / 2U) {
            ahci_serial("AHCI: IDENTIFY reports an invalid logical sector size\n");
            return -1;
        }
        d->sector_size = logical_words * 2U;
    }
    if ((id[83] & 0xC000U) == 0x4000U) {
        d->flush_ext = (id[83] & (1U << 12)) != 0U;
    }
    d->flush_supported = d->flush_ext || (id[82] & (1U << 12)) != 0U;
    return 0;
}

static int ahci_setup_dma(struct ahci_dev *d) {
    uint64_t clb_page = pmm_alloc_page();
    uint64_t ctba_page = pmm_alloc_page();
    uint64_t dma_page = pmm_alloc_page();

    if (clb_page == 0 || ctba_page == 0 || dma_page == 0) {
        if (clb_page) pmm_free_page(clb_page);
        if (ctba_page) pmm_free_page(ctba_page);
        if (dma_page) pmm_free_page(dma_page);
        return -1;
    }

    d->clb_phys  = clb_page;
    d->fb_phys   = clb_page + 0x400;
    d->ctba_phys = ctba_page;
    d->dma_phys  = dma_page;
    d->dma_virt  = (void *)(uintptr_t)vm_phys_to_virt(dma_page);

    memset((void *)(uintptr_t)vm_phys_to_virt(clb_page), 0, 0x500);
    memset((void *)(uintptr_t)vm_phys_to_virt(ctba_page), 0, 0x400);
    return 0;
}

static int ahci_stop_port(struct ahci_dev *d) {
    uint32_t *port = ahci_port_base(d->hba, d->port);
    uint32_t cmd = ahci_read32(port, AHCI_P_CMD);

    if (cmd & AHCI_PCMD_ST) {
        ahci_write32(port, AHCI_P_CMD, cmd & ~AHCI_PCMD_ST);
        for (uint64_t i = 0; i < AHCI_TIMEOUT_LOOPS; ++i) {
            if ((ahci_read32(port, AHCI_P_CMD) & AHCI_PCMD_CR) == 0U) {
                break;
            }
            io_wait();
            if (i + 1U == AHCI_TIMEOUT_LOOPS) goto timeout;
        }
    }

    cmd = ahci_read32(port, AHCI_P_CMD);
    if (cmd & AHCI_PCMD_FIS_RX) {
        ahci_write32(port, AHCI_P_CMD, cmd & ~AHCI_PCMD_FIS_RX);
        for (uint64_t i = 0; i < AHCI_TIMEOUT_LOOPS; ++i) {
            if ((ahci_read32(port, AHCI_P_CMD) & AHCI_PCMD_FR) == 0U) {
                return 0;
            }
            io_wait();
            if (i + 1U == AHCI_TIMEOUT_LOOPS) goto timeout;
        }
    }

    return 0;

timeout:
    ahci_serial("AHCI: port ");
    ahci_serial_hex32(d->port);
    ahci_serial(" command engine did not stop\n");
    return -1;
}

static int ahci_init_port(struct ahci_dev *d) {
    uint32_t *port = ahci_port_base(d->hba, d->port);
    if (ahci_stop_port(d) != 0) {
        return -1;
    }

    ahci_write32(port, AHCI_P_SERR, 0xFFFFFFFF);

    ahci_write32(port, AHCI_P_CLB,  (uint32_t)(d->clb_phys & 0xFFFFFFFF));
    ahci_write32(port, AHCI_P_CLBU, (uint32_t)(d->clb_phys >> 32));
    ahci_write32(port, AHCI_P_FB,   (uint32_t)(d->fb_phys & 0xFFFFFFFF));
    ahci_write32(port, AHCI_P_FBU,  (uint32_t)(d->fb_phys >> 32));

    ahci_write32(port, AHCI_P_IE, 0);
    ahci_write32(port, AHCI_P_IS, 0xFFFFFFFF);

    ahci_write32(port, AHCI_P_CMD,
                 AHCI_PCMD_SPIN_UP | AHCI_PCMD_POWER_ON | AHCI_PCMD_FIS_RX);
    ahci_write32(port, AHCI_P_CMD,
                 AHCI_PCMD_SPIN_UP | AHCI_PCMD_POWER_ON |
                 AHCI_PCMD_FIS_RX | AHCI_PCMD_ST);
    return 0;
}

static void ahci_free_dma(struct ahci_dev *d) {
    if (d->clb_phys != 0U) pmm_free_page(d->clb_phys);
    if (d->ctba_phys != 0U) pmm_free_page(d->ctba_phys);
    if (d->dma_phys != 0U) pmm_free_page(d->dma_phys);
    d->clb_phys = 0;
    d->ctba_phys = 0;
    d->dma_phys = 0;
    d->dma_virt = NULL;
}

static void ahci_cleanup_port(struct ahci_dev *d) {
    (void)ahci_stop_port(d);
    ahci_free_dma(d);
}

static int ahci_configure_port(struct ahci_dev *d) {
    if (ahci_setup_dma(d) != 0) {
        return -1;
    }
    if (ahci_init_port(d) != 0) {
        ahci_free_dma(d);
        return -1;
    }
    return 0;
}

static void ahci_format_name(char *out, unsigned index) {
    out[0] = 's';
    out[1] = 'a';
    out[2] = 't';
    out[3] = 'a';
    char digits[8];
    int n = 0;
    do {
        digits[n++] = (char)('0' + (index % 10U));
        index /= 10U;
    } while (index != 0 && n < 8);

    int pos = 4;
    for (int i = n - 1; i >= 0; --i) {
        out[pos++] = digits[i];
    }
    out[pos] = '\0';
}

static int ahci_register_disk(struct ahci_dev *d, unsigned index) {
    block_device *dev = (block_device *)kmalloc(sizeof(block_device));
    if (dev == NULL) {
        ahci_serial("AHCI: out of memory for block_device\n");
        return -1;
    }
    memset(dev, 0, sizeof(*dev));

    ahci_format_name(d->name, index);
    dev->name         = d->name;
    dev->sector_count = d->sector_count;
    dev->sector_size  = d->sector_size;
    dev->dev_data     = d;
    dev->read         = ahci_read_sectors;
    dev->write        = d->flush_supported ? ahci_write_sectors : NULL;
    dev->flush        = d->flush_supported ? ahci_flush_device : NULL;
    dev->next         = NULL;

    if (blockdev_register(dev) != 0) {
        ahci_serial("AHCI: blockdev registration failed for ");
        ahci_serial(d->name);
        ahci_serial("\n");
        kfree(dev);
        return -1;
    }
    return 0;
}

void ahci_init(void) {
    if (!vm_is_initialized() || !heap_is_initialized()) {
        ahci_serial("AHCI: init skipped (VMM/heap not ready)\n");
        return;
    }

    uint8_t bus, device, function;
    if (pci_find_by_class(PCI_CLASS_STORAGE, PCI_SUBCLASS_SATA,
                          PCI_PROGIF_AHCI, &bus, &device, &function) != 0) {
        ahci_serial("AHCI: no AHCI controller found\n");
        return;
    }
    ahci_serial("AHCI: controller at ");
    ahci_serial_hex32(bus);
    ahci_serial(":");
    ahci_serial_hex32(device);
    ahci_serial(".");
    ahci_serial_hex32(function);
    ahci_serial("\n");

    uint32_t class_info = pci_config_read_dword(bus, device, function, 0x08);
    ahci_serial("AHCI: PCI class/subclass/prog-if=");
    ahci_serial_hex32((class_info >> 8) & 0xFFFFFFU);
    ahci_serial("\n");

    uint64_t bar_addr = 0, bar_size = 0;
    int bar_found = -1;
    for (int i = 0; i < 6; ++i) {
        if (pci_bar_info(bus, device, function, (uint8_t)i,
                         &bar_addr, &bar_size) == 0 && bar_size >= 0x1000) {
            bar_found = i;
            break;
        }
    }
    if (bar_found < 0) {
        ahci_serial("AHCI: no usable memory BAR\n");
        return;
    }
    ahci_serial("AHCI: BAR");
    ahci_serial_hex32((uint32_t)bar_found);
    ahci_serial(" phys=");
    ahci_serial_hex64(bar_addr);
    ahci_serial(" size=");
    ahci_serial_hex64(bar_size);
    ahci_serial("\n");

    uint32_t cmd = pci_config_read_dword(bus, device, function,
                                         PCI_OFFSET_COMMAND);
    pci_config_write_dword(bus, device, function, PCI_OFFSET_COMMAND,
                           cmd | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);

    if ((bar_addr & (VMM_PAGE_SIZE - 1U)) != 0U ||
        bar_size > UINT64_MAX - (VMM_PAGE_SIZE - 1U)) {
        ahci_serial("AHCI: HBA BAR is not safely page-mappable\n");
        return;
    }
    uint64_t map_size = (bar_size + VMM_PAGE_SIZE - 1U) &
                        ~(VMM_PAGE_SIZE - 1U);
    uint64_t mapped = 0;
    for (uint64_t off = 0; off < map_size; off += VMM_PAGE_SIZE) {
        if (!vm_map_page(AHCI_MMIO_VIRT + off, bar_addr + off,
                         VMM_PAGE_PRESENT | VMM_PAGE_WRITABLE |
                         VMM_PAGE_CACHE_DISABLE | VMM_PAGE_WRITE_THROUGH)) {
            while (mapped > 0U) {
                mapped -= VMM_PAGE_SIZE;
                (void)vm_unmap_page(AHCI_MMIO_VIRT + mapped);
            }
            ahci_serial("AHCI: failed to map HBA MMIO\n");
            return;
        }
        mapped += VMM_PAGE_SIZE;
    }
    uint32_t *hba = (uint32_t *)(uintptr_t)AHCI_MMIO_VIRT;

    ahci_write32(hba, AHCI_HBA_GHC, AHCI_GHC_AE);
    ahci_write32(hba, AHCI_HBA_GHC, AHCI_GHC_HR | AHCI_GHC_AE);
    bool reset_done = false;
    for (uint64_t i = 0; i < AHCI_TIMEOUT_LOOPS; ++i) {
        if ((ahci_read32(hba, AHCI_HBA_GHC) & AHCI_GHC_HR) == 0) {
            reset_done = true;
            break;
        }
        io_wait();
    }
    if (!reset_done) {
        ahci_serial("AHCI: controller reset timed out\n");
        return;
    }
    ahci_write32(hba, AHCI_HBA_GHC, AHCI_GHC_AE);

    uint32_t cap = ahci_read32(hba, AHCI_HBA_CAP);
    uint32_t pi  = ahci_read32(hba, AHCI_HBA_PI);
    uint32_t vs  = ahci_read32(hba, AHCI_HBA_VS);
    ahci_serial("AHCI: CAP=");
    ahci_serial_hex32(cap);
    ahci_serial(" PI=");
    ahci_serial_hex32(pi);
    ahci_serial(" VS=");
    ahci_serial_hex32(vs);
    ahci_serial("\n");
    if ((cap & AHCI_CAP_S64A) == 0U) {
        ahci_serial("AHCI: controller lacks 64-bit DMA; refusing unsafe buffers\n");
        return;
    }

    unsigned port_count = (cap & 0x1F) + 1;
    if (port_count > AHCI_MAX_PORTS) port_count = AHCI_MAX_PORTS;

    unsigned disk_index = 0;
    for (unsigned p = 0; p < port_count && disk_index < AHCI_MAX_DISKS; ++p) {
        if ((pi & (1U << p)) == 0) {
            continue;
        }

        uint64_t port_end = AHCI_HBA_PORTS +
                            (uint64_t)p * AHCI_PORT_STRIDE + AHCI_PORT_STRIDE;
        if (port_end > bar_size) {
            ahci_serial("AHCI: port registers exceed BAR size; skipping port ");
            ahci_serial_hex32(p);
            ahci_serial("\n");
            continue;
        }

        uint32_t *port = ahci_port_base(hba, p);
        uint32_t ssts = ahci_read32(port, AHCI_P_SSTS);
        if ((ssts & AHCI_SSTS_DET_MASK) != AHCI_SSTS_DET_DEV) {
            ahci_serial("AHCI: port ");
            ahci_serial_hex32(p);
            ahci_serial(" empty\n");
            continue;
        }

        struct ahci_dev *d = (struct ahci_dev *)kmalloc(sizeof(*d));
        if (d == NULL) {
            ahci_serial("AHCI: out of memory for port context\n");
            continue;
        }
        memset(d, 0, sizeof(*d));
        d->hba  = hba;
        d->port = p;

        if (ahci_configure_port(d) != 0) {
            ahci_serial("AHCI: DMA allocation failed for port ");
            ahci_serial_hex32(p);
            ahci_serial("\n");
            kfree(d);
            continue;
        }

        uint32_t sig = ahci_read32(port, AHCI_P_SIG);
        if (sig != AHCI_SIG_DISK) {
            ahci_serial("AHCI: port ");
            ahci_serial_hex32(p);
            ahci_serial(" signature ");
            ahci_serial_hex32(sig);
            ahci_serial(" not an ATA disk; skipping\n");
            ahci_cleanup_port(d);
            kfree(d);
            continue;
        }

        if (ahci_identify(d) != 0) {
            ahci_serial("AHCI: IDENTIFY failed on port ");
            ahci_serial_hex32(p);
            ahci_serial("\n");
            ahci_cleanup_port(d);
            kfree(d);
            continue;
        }

        if (d->sector_size != BLOCKDEV_SECTOR_SIZE) {
            ahci_serial("AHCI: unsupported logical sector size ");
            ahci_serial_hex32(d->sector_size);
            ahci_serial(" (requires 512 bytes)\n");
            ahci_cleanup_port(d);
            kfree(d);
            continue;
        }

        if (ahci_register_disk(d, disk_index) != 0) {
            ahci_cleanup_port(d);
            kfree(d);
            continue;
        }
        ahci_serial("AHCI: registered ");
        ahci_serial(d->name);
        ahci_serial(" (port ");
        ahci_serial_hex32(p);
        ahci_serial(", sectors=");
        ahci_serial_hex64(d->sector_count);
        ahci_serial(", sector_size=");
        ahci_serial_hex32(d->sector_size);
        ahci_serial(", capacity_bytes=");
        ahci_serial_hex64(d->sector_count * d->sector_size);
        ahci_serial(")\n");
        if (d->flush_supported) {
            ahci_serial("AHCI: write and cache-flush support enabled for ");
            ahci_serial(d->name);
            ahci_serial("\n");
        } else {
            ahci_serial("AHCI: ");
            ahci_serial(d->name);
            ahci_serial(" is read-only (no ATA flush command)\n");
        }
        ++disk_index;
    }
    if (disk_index == 0U) {
        ahci_serial("AHCI: controller found but no supported SATA disks registered\n");
    }
}
