
#include "drivers/pci/pci.h"

#include <stddef.h>
#include <stdint.h>

#include "arch/x86_64/io.h"

#define PCI_CONFIG_ADDRESS  0xCF8
#define PCI_CONFIG_DATA     0xCFC
#define PCI_CONFIG_ENABLE   0x80000000UL

static uint32_t pci_make_address(uint8_t bus, uint8_t device,
                                 uint8_t function, uint8_t offset) {
    return PCI_CONFIG_ENABLE |
           ((uint32_t)bus << 16) |
           ((uint32_t)device << 11) |
           ((uint32_t)function << 8) |
           (uint32_t)(offset & 0xFC);
}

uint32_t pci_config_read_dword(uint8_t bus, uint8_t device,
                               uint8_t function, uint8_t offset) {
    outl(PCI_CONFIG_ADDRESS, pci_make_address(bus, device, function, offset));
    return inl(PCI_CONFIG_DATA);
}

void pci_config_write_dword(uint8_t bus, uint8_t device,
                            uint8_t function, uint8_t offset,
                            uint32_t value) {
    outl(PCI_CONFIG_ADDRESS, pci_make_address(bus, device, function, offset));
    outl(PCI_CONFIG_DATA, value);
}

uint8_t pci_config_read_byte(uint8_t bus, uint8_t device,
                             uint8_t function, uint8_t offset) {
    uint32_t dword = pci_config_read_dword(bus, device, function, offset & 0xFC);
    return (uint8_t)((dword >> ((offset & 3) * 8)) & 0xFF);
}

int pci_find_by_class(uint8_t base_class, uint8_t subclass, uint8_t prog_if,
                      uint8_t *bus, uint8_t *device, uint8_t *function) {
    for (uint8_t b = 0; b < 255; ++b) {
        for (uint8_t d = 0; d < 32; ++d) {
            for (uint8_t f = 0; f < 8; ++f) {
                uint32_t id = pci_config_read_dword(b, d, f, PCI_OFFSET_VENDOR);
                if (id == 0xFFFFFFFFU || (id & 0xFFFF) == 0xFFFFU) {

                    if (f == 0) break;
                    continue;
                }

                uint32_t class_dw = pci_config_read_dword(b, d, f, 0x08);

                uint8_t base     = (uint8_t)((class_dw >> 24) & 0xFF);
                uint8_t subclass_ = (uint8_t)((class_dw >> 16) & 0xFF);
                uint8_t prog_if_  = (uint8_t)((class_dw >> 8) & 0xFF);

                if (base == base_class && subclass_ == subclass &&
                    (prog_if == 0xFF || prog_if_ == prog_if)) {
                    *bus = b;
                    *device = d;
                    *function = f;
                    return 0;
                }
            }
        }
    }
    return -1;
}

int pci_bar_info(uint8_t bus, uint8_t device, uint8_t function,
                 uint8_t bar_index, uint64_t *address, uint64_t *size) {
    if (bar_index > 5 || address == NULL || size == NULL) {
        return -1;
    }

    uint8_t offset = PCI_OFFSET_BAR0 + bar_index * 4;
    uint32_t bar = pci_config_read_dword(bus, device, function, offset);

    if (bar == 0 || (bar & 0x1) != 0) {
        return -1;
    }

    int is64 = ((bar & 0x6) == 0x6);
    *address = (uint64_t)(bar & ~0xFULL);

    if (is64) {
        uint32_t hi = pci_config_read_dword(bus, device, function, offset + 4);
        *address |= ((uint64_t)hi << 32);
    }

    uint32_t orig_lo = bar;
    uint32_t orig_hi = is64 ? pci_config_read_dword(bus, device, function, offset + 4) : 0;

    pci_config_write_dword(bus, device, function, offset, 0xFFFFFFFFU);
    if (is64) {
        pci_config_write_dword(bus, device, function, offset + 4, 0xFFFFFFFFU);
    }
    uint32_t mask_lo = pci_config_read_dword(bus, device, function, offset);
    uint32_t mask_hi = is64 ? pci_config_read_dword(bus, device, function, offset + 4) : 0;

    pci_config_write_dword(bus, device, function, offset, orig_lo);
    if (is64) {
        pci_config_write_dword(bus, device, function, offset + 4, orig_hi);
    }

    if (is64) {
        uint64_t mask = ((uint64_t)mask_hi << 32) | (uint64_t)(mask_lo & 0xFFFFFFF0U);
        *size = (~mask) + 1ULL;
    } else {
        uint32_t mask32 = mask_lo & 0xFFFFFFF0U;
        *size = (uint64_t)(~mask32) + 1ULL;
    }
    if (*size == 0) {
        return -1;
    }
    return 0;
}
