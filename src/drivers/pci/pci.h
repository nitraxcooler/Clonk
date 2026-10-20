#ifndef PCI_H
#define PCI_H

#include <stdint.h>

#define PCI_OFFSET_VENDOR       0x00
#define PCI_OFFSET_COMMAND      0x04
#define PCI_OFFSET_STATUS       0x06
#define PCI_OFFSET_PROG_IF      0x09
#define PCI_OFFSET_SUBCLASS     0x0A
#define PCI_OFFSET_BASECLASS    0x0B
#define PCI_OFFSET_BAR0         0x10
#define PCI_OFFSET_BAR5         0x24

#define PCI_COMMAND_MEMORY      0x02
#define PCI_COMMAND_MASTER      0x04

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

int pci_find_by_class(uint8_t base_class, uint8_t subclass, uint8_t prog_if,
                      uint8_t *bus, uint8_t *device, uint8_t *function);

int pci_bar_info(uint8_t bus, uint8_t device, uint8_t function,
                 uint8_t bar_index, uint64_t *address, uint64_t *size);

#endif
