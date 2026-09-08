/*
 * MyOS AHCI (Advanced Host Controller Interface) SATA driver.
 *
 * What is AHCI?
 *   AHCI is the host-controller register interface for SATA.  The OS finds
 *   an AHCI controller as a PCI mass-storage device, maps its memory-mapped
 *   register set (HBA), and drives each SATA port by handing the controller
 *   DMA structures (a command list, command tables and FIS receive buffer)
 *   that describe ATA commands.  The controller performs the DMA itself —
 *   the CPU only writes a few registers and then polls for completion.
 *
 * How this driver works:
 *   1. pci_find_by_class() locates a 01/06/01 (mass storage / SATA / AHCI)
 *      device and maps its HBA MMIO BAR into the kernel's virtual space.
 *   2. The HBA is taken out of reset and placed in AHCI mode (GHC.AE).
 *   3. For every implemented port with a device attached (PxSSTS.DET == 3
 *      and a PxSIG of 0x00000101, i.e. an ATA disk), we allocate a
 *      physically-contiguous command list+FIS page, a command-table page and
 *      a 4 KiB DMA bounce buffer.
 *   4. IDENTIFY DEVICE returns the disk geometry (sector count in words
 *      60/61 LBA28 and 100..103 LBA48).
 *   5. Reads use READ DMA EXT (0x25) into the bounce buffer; the blockdev
 *      request buffers are copied from it.  A bounce buffer is used because
 *      the kernel heap is built from scattered 4 KiB physical pages and one
 *      simple PRDT entry requires one physically contiguous target.
 *
 * Limitations (deliberate, per milestone):
 *   - read-only: the block_device write callback is NULL
 *   - no NCQ (single command slot, slot 0)
 *   - no port multipliers, no ATAPI/CD-ROM devices
 *   - polling only (no IRQ/APIC wiring of the HBA interrupt)
 */

#include "drivers/storage/ahci.h"

#include <stddef.h>
#include <stdint.h>

#include "arch/x86_64/io.h"
#include "drivers/pci/pci.h"
#include "kernel/blockdev.h"
#include "kernel/heap.h"
#include "kernel/pmm.h"
#include "kernel/vmm.h"
#include "lib/mem.h"

/* ------------------------- HBA register map ------------------------- */

#define AHCI_HBA_CAP    0x00    /* capabilities                       */
#define AHCI_HBA_GHC    0x04    /* global host control                */
#define AHCI_HBA_IS     0x08    /* interrupt status                   */
#define AHCI_HBA_PI     0x0C    /* ports implemented                  */
#define AHCI_HBA_VS     0x10    /* version                            */

#define AHCI_HBA_PORTS  0x100   /* first port register block          */
#define AHCI_PORT_STRIDE 0x80

#define AHCI_GHC_HR     (1U << 0)   /* HBA reset (self-clearing) */
#define AHCI_GHC_IE     (1U << 1)   /* global interrupt enable   */
#define AHCI_GHC_AE     (1U << 31)  /* AHCI enable               */

/* Per-port register offsets (relative to the port's base). */
#define AHCI_P_CLB      0x00    /* command list base (low)  */
#define AHCI_P_CLBU     0x04    /* command list base (high) */
#define AHCI_P_FB       0x08    /* FIS buffer base (low)    */
#define AHCI_P_FBU      0x0C    /* FIS buffer base (high)   */
#define AHCI_P_IS       0x10    /* interrupt status (RW1C)  */
#define AHCI_P_IE       0x14    /* interrupt enable         */
#define AHCI_P_CMD      0x18    /* port command             */
#define AHCI_P_TFD      0x20    /* task file data           */
#define AHCI_P_SIG      0x24    /* signature                */
#define AHCI_P_SSTS     0x28    /* SATA status (SCR)        */
#define AHCI_P_SCTL     0x2C    /* SATA control (SCR)       */
#define AHCI_P_SERR     0x30    /* SATA error (SCR, RW1C)   */
#define AHCI_P_CI       0x38    /* command issue            */

#define AHCI_PCMD_ST       (1U << 0)   /* start command DMA engine  */
#define AHCI_PCMD_SPIN_UP  (1U << 1)   /* spin up device            */
#define AHCI_PCMD_POWER_ON (1U << 2)   /* power up device           */
#define AHCI_PCMD_FIS_RX   (1U << 4)   /* enable FIS receive engine */
#define AHCI_PCMD_FR       (1U << 14)  /* FIS receive running       */
#define AHCI_PCMD_CR       (1U << 15)  /* command list running      */

/* PxIS bits (polling driver only looks at the error/completion ones). */
#define AHCI_PIS_DHRS (1U << 0)   /* D2H register FIS received */
#define AHCI_PIS_PSS  (1U << 1)   /* PIO setup FIS received    */
#define AHCI_PIS_UFS  (1U << 4)   /* unknown FIS error         */
#define AHCI_PIS_DPS  (1U << 5)   /* descriptor processed (sg) */
#define AHCI_PIS_OFS  (1U << 24)  /* overflow                  */
#define AHCI_PIS_INFS (1U << 26)  /* interface non-fatal error */
#define AHCI_PIS_IFS  (1U << 27)  /* interface fatal error     */
#define AHCI_PIS_HBDS (1U << 28)  /* host bus data error      */
#define AHCI_PIS_HBFS (1U << 29)  /* host bus fatal error     */
#define AHCI_PIS_TFES (1U << 30)  /* task file error          */

#define AHCI_PIS_ERROR (AHCI_PIS_UFS | AHCI_PIS_DPS | AHCI_PIS_OFS | \
                        AHCI_PIS_INFS | AHCI_PIS_IFS | AHCI_PIS_HBDS | \
                        AHCI_PIS_HBFS | AHCI_PIS_TFES)

#define AHCI_SIG_DISK   0x00000101U   /* ATA disk signature           */
#define AHCI_SIG_CDROM  0xEB140101U
#define AHCI_SSTS_DET_MASK 0xFU
#define AHCI_SSTS_DET_DEV  3U          /* device present, PHY up */
/* ----------------------- DMA data structures ------------------------ */

/* 32-byte AHCI command header (one entry per command slot). */
struct ahci_cmd_hdr {
    uint16_t opts;      /* bits 0-4: FIS length in dwords                */
    uint16_t prdtl;     /* number of PRDE entries in the command table   */
    uint32_t prdbc;     /* bytes transferred (written by the controller) */
    uint64_t ctba;      /* physical address of the command table         */
    uint32_t reserved[4];
} __attribute__((packed));

/* 16-byte physical-region-descriptor table entry. */
struct ahci_prd {
    uint64_t dba;       /* physical address of the data buffer           */
    uint32_t reserved;
    uint32_t flags;     /* bits 0-21: byte count - 1; bit 31: interrupt  */
} __attribute__((packed));

#define AHCI_CMD_TABLE_PRDT_OFFSET 0x80    /* PRDT starts after the FIS */

/* Per-disk driver context. */
#define AHCI_MAX_PORTS        32
#define AHCI_MAX_DISKS        8
#define AHCI_CMD_SLOT         0
#define AHCI_DMA_SECTORS      8            /* 8 * 512 = 4096 B bounce    */
#define AHCI_TIMEOUT_LOOPS    10000000ULL  /* bounded poll (with io_wait) */

struct ahci_dev {
    uint32_t *hba;          /* kernel-virtual mapping of the HBA MMIO */
    unsigned  port;         /* HBA port index                         */
    uint64_t  sector_count; /* from IDENTIFY                             */

    /* Physical page allocations (usable RAM, mapped by HHDM). */
    uint64_t  clb_phys;     /* page A: command list   (0x000..0x400)   */
    uint64_t  fb_phys;      /*         FIS buffer     (0x400..0x500)   */
    uint64_t  ctba_phys;    /* page B: command table  (FIS + PRDT)     */
    uint64_t  dma_phys;     /* page C: DMA bounce buffer                */
    void     *dma_virt;     /* HHDM virtual view of the bounce buffer   */

    char      name[16];     /* "sataN"                                   */
};

/* Fixed kernel-virtual window where the HBA MMIO is mapped.  It lives
 * above the kernel image and the heap arena, far from any other mapping. */
#define AHCI_MMIO_VIRT 0xffffffffff000000ULL

/* --------------------------- serial logging ------------------------- */

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

/* --------------------------- register I/O --------------------------- */

static inline uint32_t ahci_read32(uint32_t *base, uint32_t offset) {
    return *(volatile uint32_t *)((uintptr_t)base + offset);
}

static inline void ahci_write32(uint32_t *base, uint32_t offset, uint32_t value) {
    *(volatile uint32_t *)((uintptr_t)base + offset) = value;
}

static inline uint32_t *ahci_port_base(uint32_t *hba, unsigned port) {
    return (uint32_t *)((uintptr_t)hba + AHCI_HBA_PORTS + port * AHCI_PORT_STRIDE);
}
/* --------------------------- command helpers ------------------------ */

/* Build the slot-0 command header + command table + PRDT.  `command` is
 * the ATA opcode, `lba`/`sectors` the LBA48 block range (ignored for
 * non-LBA commands), and `bytes` the PRDT byte count. */
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

    hdr->opts  = 5;                    /* 20-byte H2D FIS = 5 dwords */
    hdr->prdtl = 1;
    hdr->prdbc = 0;
    hdr->ctba  = d->ctba_phys;

    /* Register H2D FIS. */
    fis[0]  = 0x27;                    /* FIS type: register H2D      */
    fis[1]  = 0x80;                    /* bit 7: update command reg   */
    fis[2]  = command;                 /* ATA opcode                  */
    fis[3]  = 0;                       /* feature low                 */
    fis[4]  = (uint8_t)(lba & 0xFF);   /* LBA 7:0                     */
    fis[5]  = (uint8_t)((lba >> 8) & 0xFF);   /* LBA 15:8          */
    fis[6]  = (uint8_t)((lba >> 16) & 0xFF);  /* LBA 23:16         */
    fis[7]  = 0x40;                    /* device: LBA bit 6 set       */
    fis[8]  = (uint8_t)((lba >> 24) & 0xFF);  /* LBA 31:24         */
    fis[9]  = (uint8_t)((lba >> 32) & 0xFF);  /* LBA 39:32         */
    fis[10] = (uint8_t)((lba >> 40) & 0xFF);  /* LBA 47:40         */
    fis[11] = 0;                       /* feature high                */
    fis[12] = (uint8_t)(sectors & 0xFF);         /* sector count low  */
    fis[13] = (uint8_t)((sectors >> 8) & 0xFF);  /* sector count high */

    prd[0].dba   = d->dma_phys;
    prd[0].flags = (bytes - 1) & 0x3FFFFF;   /* DBC = bytes - 1 */
}

static void ahci_issue_slot(struct ahci_dev *d) {
    uint32_t *port = ahci_port_base(d->hba, d->port);
    ahci_write32(port, AHCI_P_IS, 0xFFFFFFFF);           /* clear stale IRQ */
    ahci_write32(port, AHCI_P_CI, 1U << AHCI_CMD_SLOT);  /* issue slot 0    */
}
/* Poll until slot 0 completes.  Returns 0 on success.  Every iteration
 * performs an I/O instruction, which also lets an emulator (QEMU) service
 * its DMA backends between polls. */
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
            /* Command engine finished; wait for BSY/DRQ to clear. */
            uint32_t tfd = ahci_read32(port, AHCI_P_TFD);
            if ((tfd & 0x88) == 0) {
                ahci_write32(port, AHCI_P_IS, ahci_read32(port, AHCI_P_IS));
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

/* --------------------------- block device API ------------------------ */

static int ahci_read_sectors(void *dev_data, uint64_t lba, uint32_t count,
                             void *buffer) {
    struct ahci_dev *d = (struct ahci_dev *)dev_data;
    uint8_t *out = (uint8_t *)buffer;

    while (count > 0) {
        uint32_t chunk = count > AHCI_DMA_SECTORS ? AHCI_DMA_SECTORS : count;

        ahci_fill_command(d, 0x25 /* READ DMA EXT */, lba, (uint16_t)chunk,
                          chunk * BLOCKDEV_SECTOR_SIZE);
        ahci_issue_slot(d);
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

/* --------------------------- IDENTIFY ------------------------------- */

static int ahci_identify(struct ahci_dev *d) {
    ahci_fill_command(d, 0xEC /* IDENTIFY DEVICE */, 0, 0, 512);
    ahci_issue_slot(d);
    if (ahci_wait_slot(d) != 0) {
        return -1;
    }

    const uint16_t *id = (const uint16_t *)d->dma_virt;
    if (id[0] == 0 || id[0] == 0xFFFF) {
        ahci_serial("AHCI: IDENTIFY returned no drive signature\n");
        return -1;
    }

    uint64_t lba48 = (uint64_t)id[100] | ((uint64_t)id[101] << 16) |
                     ((uint64_t)id[102] << 32) | ((uint64_t)id[103] << 48);
    uint64_t lba28 = (uint64_t)id[60] | ((uint64_t)id[61] << 16);

    d->sector_count = (lba48 != 0) ? lba48 : lba28;
    if (d->sector_count == 0) {
        ahci_serial("AHCI: IDENTIFY reports zero sectors\n");
        return -1;
    }
    return 0;
}

/* --------------------------- port setup ----------------------------- */

/* Allocate the three DMA pages and remember both physical (for the
 * controller) and virtual (for the software) addresses. */
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
    d->fb_phys   = clb_page + 0x400;   /* FIS buffer inside page A */
    d->ctba_phys = ctba_page;
    d->dma_phys  = dma_page;
    d->dma_virt  = (void *)(uintptr_t)vm_phys_to_virt(dma_page);

    memset((void *)(uintptr_t)vm_phys_to_virt(clb_page), 0, 0x500);
    memset((void *)(uintptr_t)vm_phys_to_virt(ctba_page), 0, 0x400);
    return 0;
}

/* Enable a single port: stop any running engines, program the DMA bases,
 * start the FIS receive + command engines. */
static void ahci_init_port(struct ahci_dev *d) {
    uint32_t *port = ahci_port_base(d->hba, d->port);
    uint32_t cmd = ahci_read32(port, AHCI_P_CMD);

    /* Stop running engines (CR/FR are the controller-set status bits). */
    if (cmd & (AHCI_PCMD_ST | AHCI_PCMD_FIS_RX)) {
        ahci_write32(port, AHCI_P_CMD, cmd & ~(AHCI_PCMD_ST | AHCI_PCMD_FIS_RX));
        for (uint64_t i = 0; i < AHCI_TIMEOUT_LOOPS; ++i) {
            cmd = ahci_read32(port, AHCI_P_CMD);
            if ((cmd & (AHCI_PCMD_CR | AHCI_PCMD_FR)) == 0) break;
            io_wait();
        }
    }

    ahci_write32(port, AHCI_P_SERR, 0xFFFFFFFF);   /* clear SError (RW1C) */

    ahci_write32(port, AHCI_P_CLB,  (uint32_t)(d->clb_phys & 0xFFFFFFFF));
    ahci_write32(port, AHCI_P_CLBU, (uint32_t)(d->clb_phys >> 32));
    ahci_write32(port, AHCI_P_FB,   (uint32_t)(d->fb_phys & 0xFFFFFFFF));
    ahci_write32(port, AHCI_P_FBU,  (uint32_t)(d->fb_phys >> 32));

    ahci_write32(port, AHCI_P_IE, 0);              /* polling: no IRQs */
    ahci_write32(port, AHCI_P_IS, 0xFFFFFFFF);     /* clear PxIS      */

    /* Power up, spin up, then enable FIS receive and the command engine. */
    ahci_write32(port, AHCI_P_CMD,
                 AHCI_PCMD_SPIN_UP | AHCI_PCMD_POWER_ON | AHCI_PCMD_FIS_RX);
    ahci_write32(port, AHCI_P_CMD,
                 AHCI_PCMD_SPIN_UP | AHCI_PCMD_POWER_ON |
                 AHCI_PCMD_FIS_RX | AHCI_PCMD_ST);
}
/* --------------------------- registration --------------------------- */

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

static void ahci_register_disk(struct ahci_dev *d, unsigned index) {
    block_device *dev = (block_device *)kmalloc(sizeof(block_device));
    if (dev == NULL) {
        ahci_serial("AHCI: out of memory for block_device\n");
        return;
    }
    memset(dev, 0, sizeof(*dev));

    ahci_format_name(d->name, index);
    dev->name         = d->name;
    dev->sector_count = d->sector_count;
    dev->sector_size  = BLOCKDEV_SECTOR_SIZE;
    dev->dev_data     = d;
    dev->read         = ahci_read_sectors;
    dev->write        = NULL;   /* read-only for now */
    dev->next         = NULL;

    if (blockdev_register(dev) != 0) {
        ahci_serial("AHCI: blockdev registration failed for ");
        ahci_serial(d->name);
        ahci_serial("\n");
    }
}

/* --------------------------- controller init ------------------------ */

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

    /* Locate the HBA memory BAR (any sizeable memory BAR will do). */
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

    /* Enable access to the device's memory space and bus mastering. */
    uint32_t cmd = pci_config_read_dword(bus, device, function,
                                         PCI_OFFSET_COMMAND);
    pci_config_write_dword(bus, device, function, PCI_OFFSET_COMMAND,
                           cmd | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);

    /* Map the HBA MMIO region (uncached) into the kernel address space. */
    uint64_t map_size = bar_size < 0x1000 ? 0x1000 : bar_size;
    for (uint64_t off = 0; off < map_size; off += VMM_PAGE_SIZE) {
        if (!vm_map_page(AHCI_MMIO_VIRT + off, bar_addr + off,
                         VMM_PAGE_PRESENT | VMM_PAGE_WRITABLE |
                         VMM_PAGE_CACHE_DISABLE)) {
            ahci_serial("AHCI: failed to map HBA MMIO\n");
            return;
        }
    }
    uint32_t *hba = (uint32_t *)(uintptr_t)AHCI_MMIO_VIRT;

    /* Put the HBA into AHCI mode and issue a controller reset. */
    ahci_write32(hba, AHCI_HBA_GHC, AHCI_GHC_AE);
    ahci_write32(hba, AHCI_HBA_GHC, AHCI_GHC_HR | AHCI_GHC_AE);
    for (uint64_t i = 0; i < AHCI_TIMEOUT_LOOPS; ++i) {
        if ((ahci_read32(hba, AHCI_HBA_GHC) & AHCI_GHC_HR) == 0) break;
        io_wait();
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

    unsigned port_count = (cap & 0x1F) + 1;
    if (port_count > AHCI_MAX_PORTS) port_count = AHCI_MAX_PORTS;

    unsigned disk_index = 0;
    for (unsigned p = 0; p < port_count && disk_index < AHCI_MAX_DISKS; ++p) {
        if ((pi & (1U << p)) == 0) {
            continue;   /* port not implemented */
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

        if (ahci_setup_dma(d) != 0) {
            ahci_serial("AHCI: DMA allocation failed for port ");
            ahci_serial_hex32(p);
            ahci_serial("\n");
            kfree(d);
            continue;
        }

        ahci_init_port(d);

        /* Only plain ATA disks are supported right now. */
        uint32_t sig = ahci_read32(port, AHCI_P_SIG);
        if (sig != AHCI_SIG_DISK) {
            ahci_serial("AHCI: port ");
            ahci_serial_hex32(p);
            ahci_serial(" signature ");
            ahci_serial_hex32(sig);
            ahci_serial(" not an ATA disk; skipping\n");
            kfree(d);
            continue;
        }

        if (ahci_identify(d) != 0) {
            ahci_serial("AHCI: IDENTIFY failed on port ");
            ahci_serial_hex32(p);
            ahci_serial("\n");
            kfree(d);
            continue;
        }

        ahci_register_disk(d, disk_index);
        ahci_serial("AHCI: registered ");
        ahci_serial(d->name);
        ahci_serial(" (port ");
        ahci_serial_hex32(p);
        ahci_serial(", sectors=");
        ahci_serial_hex64(d->sector_count);
        ahci_serial(")\n");
        ++disk_index;
    }
}