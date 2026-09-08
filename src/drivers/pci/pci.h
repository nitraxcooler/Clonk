#ifndef PCI_H
#define PCI_H

#include <stdint.h>

/*
 * Minimal x86-64 PCI configuration-space access for MyOS.
 *
 * Uses the legacy I/O-port mechanism (config address 0xCF8 / data 0xCFC),
 * which works for every PCI host bridge MyOS currently targets (including
 * the QEMU virtio/ich9 setups).  Only configuration reads/writes live
 * here — no BAR assignment, no MSI/MSI-X, no hotplug.
 */

/* Offsets in PCI configuration space. */
#define PCI_OFFSET_VENDOR       0x00
#define PCI_OFFSET_COMMAND      0x04
#define PCI_OFFSET_STATUS       0x06
#define PCI_OFFSET_PROG_IF      0x09
#define PCI_OFFSET_SUBCLASS     0x0A
#define PCI_OFFSET_BASECLASS    0x0B
#define PCI_OFFSET_BAR0         0x10
#define PCI_OFFSET_BAR5         0x24

/* Command-register bits. */
#define PCI_COMMAND_MEMORY      0x02    /* respond to memory BARs       */
#define PCI_COMMAND_MASTER      0x04    /* bus-master (DMA) capable     */

/* Well-known PCI class codes. */
#define PCI_CLASS_STORAGE       0x01
#define PCI_SUBCLASS_SATA       0x06
#define PCI_PROGIF_AHCI         0x01

uint32_t pci_config_read_dword(uint8_t bus, uint8_t device,
                               uint8_t function, uint8_t offset);
void     pci_config_write_dword(uint8_t bus, uint8_t device,
                                uint8_t function, uint8_t offset,
                                uint32_t value);
uint8_t  pci_config_read_byte(uint8_t bus, uint8_t device,
                              uint8_t function, uint8_t offset);

/* Find the first PCI device whose class/subclass match (prog_if of 0xFF
 * matches any programming interface).  Returns 0 on success and fills the
 * bus/device/function triple. */
int pci_find_by_class(uint8_t base_class, uint8_t subclass, uint8_t prog_if,
                      uint8_t *bus, uint8_t *device, uint8_t *function);

/* Decode a BAR's 64-bit address and size in bytes.  Returns 0 on success.
 * Only memory-space BARs are accepted (I/O BARs return -1).  Size probing
 * temporarily overwrites the BAR so callers should not use this while the
 * device is actively being driven. */
int pci_bar_info(uint8_t bus, uint8_t device, uint8_t function,
                 uint8_t bar_index, uint64_t *address, uint64_t *size);

#endif