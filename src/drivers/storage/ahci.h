#ifndef AHCI_H
#define AHCI_H

/*
 * MyOS AHCI (Advanced Host Controller Interface) SATA driver.
 *
 * The driver currently provides:
 *   - PCI detection of an AHCI controller
 *   - controller + per-port initialisation
 *   - SATA port enumeration (PxSSTS / PxSIG)
 *   - device IDENTIFY (sector count)
 *   - read-only LBA48 DMA sector reads (READ DMA EXT)
 *
 * Everything is published through the generic block_device interface
 * (see kernel/blockdev.h); writing and NCQ are not implemented yet.
 */

/* Driver entry point — scans PCI, initialises the controller and registers
 * each attached disk with the block-device layer.  Called from
 * blockdev_init(). */
void ahci_init(void);

#endif