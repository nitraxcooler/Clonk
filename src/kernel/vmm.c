#include "kernel/vmm.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "boot/limine.h"
#include "kernel/panic.h"
#include "kernel/pmm.h"
#include "arch/x86_64/io.h"
#include "arch/x86_64/gdt.h"

/* Serial helpers so VMM progress is visible in the boot log. */
static inline void vmm_serial_putc(char c) { outb(0x3F8, (uint8_t)c); }
static void vmm_serial_write(const char *s) { while (*s) vmm_serial_putc(*s++); }
static void vmm_serial_hex64(uint64_t v) {
    const char *digits = "0123456789ABCDEF";
    vmm_serial_write("0x");
    for (int i = 15; i >= 0; --i) {
        uint8_t nibble = (uint8_t)((v >> (i * 4)) & 0xF);
        vmm_serial_putc(digits[nibble]);
    }
    vmm_serial_putc('\n');
}

#define PAGE_TABLE_ENTRIES 512U

extern uint8_t __kernel_start[];
extern uint8_t __kernel_end[];
extern uint8_t __kernel_text_start[];
extern uint8_t __kernel_text_end[];

extern volatile struct limine_hhdm_request limine_hhdm_request;
extern volatile struct limine_memmap_request limine_memmap_request;
extern volatile struct limine_executable_address_request limine_executable_address_request;

static uint64_t vmm_hhdm_offset = 0;
static uint64_t vmm_active_pml4 = 0;
static uint64_t vmm_mapped_pages = 0;
static uint64_t vmm_page_table_count = 0;
static bool vmm_initialized = false;

static uint64_t vmm_phys_to_virt(uint64_t physical_address) {
    return physical_address + vmm_hhdm_offset;
}

static void vmm_zero_memory(uint8_t *ptr, size_t length) {
    for (size_t i = 0; i < length; ++i) {
        ptr[i] = 0;
    }
}

/* On x86-64, a canonical address has bits 63:48 matching bit 47. */
static bool vmm_is_canonical(uint64_t address) {
    uint64_t upper = address & 0xFFFF000000000000ULL;
    uint64_t sign  = address & 0x0000800000000000ULL;

    if (upper == 0x0000000000000000ULL) {
        return sign == 0;
    }
    if (upper == 0xFFFF000000000000ULL) {
        return sign != 0;
    }
    return false;
}

static bool vmm_is_page_aligned(uint64_t address) {
    return (address & (VMM_PAGE_SIZE - 1U)) == 0U;
}

/* Get the next-level page table for an entry, allocating a fresh one
 * from the PMM if it doesn't exist yet. Returns NULL if the entry is a
 * huge page, since you can't descend into a huge page. */
static uint64_t *vmm_get_or_create_table(uint64_t *parent_table, uint64_t index,
                                         uint64_t *created_physical) {
    *created_physical = 0;
    uint64_t entry = parent_table[index];

    if ((entry & VMM_PAGE_PRESENT) != 0U) {
        if ((entry & VMM_PAGE_LARGE) != 0U) {
            return NULL; /* it's a huge page — nothing to descend into */
        }
        return (uint64_t *)vmm_phys_to_virt(entry & VMM_PAGE_ADDRESS_MASK);
    }

    uint64_t physical_address = pmm_alloc_page();
    if (physical_address == 0U) {
        return NULL;
    }

    uint64_t *table = (uint64_t *)vmm_phys_to_virt(physical_address);
    vmm_zero_memory((uint8_t *)table, VMM_PAGE_SIZE);
    parent_table[index] = physical_address | VMM_PAGE_PRESENT | VMM_PAGE_WRITABLE;
    ++vmm_page_table_count;
    *created_physical = physical_address;
    return table;
}

static void vmm_rollback_tables(uint64_t **entries, const uint64_t *physical_pages,
                                size_t count) {
    while (count > 0U) {
        --count;
        *entries[count] = 0;
        pmm_free_page(physical_pages[count]);
        --vmm_page_table_count;
    }
}

/* Walk the whole page-table tree and count mapped pages and page-table
 * pages, including huge pages set up by the bootloader. */
static void vmm_recount(void) {
    vmm_mapped_pages = 0;
    vmm_page_table_count = 0;

    if (vmm_active_pml4 == 0U) {
        return;
    }

    uint64_t *pml4 = (uint64_t *)vmm_phys_to_virt(vmm_active_pml4);
    ++vmm_page_table_count; /* don't forget the PML4 itself */

    for (uint64_t i = 0; i < PAGE_TABLE_ENTRIES; ++i) {
        uint64_t pml4_entry = pml4[i];
        if ((pml4_entry & VMM_PAGE_PRESENT) == 0U) {
            continue;
        }

        uint64_t *pdpt = (uint64_t *)vmm_phys_to_virt(pml4_entry & VMM_PAGE_ADDRESS_MASK);
        ++vmm_page_table_count;

        for (uint64_t j = 0; j < PAGE_TABLE_ENTRIES; ++j) {
            uint64_t pdpt_entry = pdpt[j];
            if ((pdpt_entry & VMM_PAGE_PRESENT) == 0U) {
                continue;
            }

            if ((pdpt_entry & VMM_PAGE_LARGE) != 0U) {
                /* A 1 GiB page counts as 262144 normal pages. */
                vmm_mapped_pages += 512U * 512U;
                continue;
            }

            uint64_t *pd = (uint64_t *)vmm_phys_to_virt(pdpt_entry & VMM_PAGE_ADDRESS_MASK);
            ++vmm_page_table_count;

            for (uint64_t k = 0; k < PAGE_TABLE_ENTRIES; ++k) {
                uint64_t pd_entry = pd[k];
                if ((pd_entry & VMM_PAGE_PRESENT) == 0U) {
                    continue;
                }

                if ((pd_entry & VMM_PAGE_LARGE) != 0U) {
                    /* A 2 MiB page counts as 512 normal pages. */
                    vmm_mapped_pages += 512U;
                    continue;
                }

                uint64_t *pt = (uint64_t *)vmm_phys_to_virt(pd_entry & VMM_PAGE_ADDRESS_MASK);
                ++vmm_page_table_count;

                for (uint64_t l = 0; l < PAGE_TABLE_ENTRIES; ++l) {
                    if ((pt[l] & VMM_PAGE_PRESENT) != 0U) {
                        ++vmm_mapped_pages;
                    }
                }
            }
        }
    }
}

uint64_t vm_create_address_space(void) {
    uint64_t pml4_physical = pmm_alloc_page();
    if (pml4_physical == 0U) {
        return 0;
    }

    uint64_t *pml4 = (uint64_t *)vmm_phys_to_virt(pml4_physical);
    vmm_zero_memory((uint8_t *)pml4, VMM_PAGE_SIZE);
    if (vmm_active_pml4 != 0U) {
        uint64_t *kernel_pml4 = (uint64_t *)vmm_phys_to_virt(vmm_active_pml4);
        for (size_t i = 256; i < PAGE_TABLE_ENTRIES; ++i) {
            pml4[i] = kernel_pml4[i] & ~VMM_PAGE_USER;
        }
    }
    return pml4_physical;
}

void vm_switch_address_space(uint64_t pml4_physical_address) {
    if (pml4_physical_address == 0U || !vmm_is_page_aligned(pml4_physical_address)) {
        kernel_panic("INVALID_PAGE_TABLE");
    }

    vmm_active_pml4 = pml4_physical_address;
    /* Writing CR3 switches the address space and clears the TLB,
     * so the new tables are in effect right away. */
    __asm__ volatile ("mov %0, %%cr3" : : "r"(pml4_physical_address) : "memory");
    __asm__ volatile ("mov %%cr3, %%rax\n\tmov %%rax, %%cr3" : : : "rax");
}

uint64_t vm_get_active_pml4(void) {
    return vmm_active_pml4;
}

uint64_t vm_get_cr3(void) {
    uint64_t cr3 = 0;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

bool vm_is_initialized(void) {
    return vmm_initialized;
}

static void vmm_make_kernel_text_executable(uint64_t pml4_physical_address) {
    uint64_t start = (uint64_t)(uintptr_t)__kernel_text_start & ~0xFFFULL;
    uint64_t end = ((uint64_t)(uintptr_t)__kernel_text_end + 0xFFFULL) & ~0xFFFULL;
    for (uint64_t address = start; address < end; address += VMM_PAGE_SIZE) {
        uint64_t *pml4 = (uint64_t *)vmm_phys_to_virt(pml4_physical_address);
        uint64_t *pml4_entry = &pml4[(address >> 39) & 0x1FFU];
        if ((*pml4_entry & VMM_PAGE_PRESENT) == 0U) kernel_panic("KERNEL_TEXT_UNMAPPED");
        *pml4_entry &= ~VMM_PAGE_NX;
        uint64_t *pdpt = (uint64_t *)vmm_phys_to_virt(*pml4_entry & VMM_PAGE_ADDRESS_MASK);
        uint64_t *pdpt_entry = &pdpt[(address >> 30) & 0x1FFU];
        if ((*pdpt_entry & VMM_PAGE_PRESENT) == 0U) kernel_panic("KERNEL_TEXT_UNMAPPED");
        *pdpt_entry &= ~VMM_PAGE_NX;
        if ((*pdpt_entry & VMM_PAGE_LARGE) != 0U) continue;
        uint64_t *pd = (uint64_t *)vmm_phys_to_virt(*pdpt_entry & VMM_PAGE_ADDRESS_MASK);
        uint64_t *pd_entry = &pd[(address >> 21) & 0x1FFU];
        if ((*pd_entry & VMM_PAGE_PRESENT) == 0U) kernel_panic("KERNEL_TEXT_UNMAPPED");
        *pd_entry &= ~VMM_PAGE_NX;
        if ((*pd_entry & VMM_PAGE_LARGE) != 0U) continue;
        uint64_t *pt = (uint64_t *)vmm_phys_to_virt(*pd_entry & VMM_PAGE_ADDRESS_MASK);
        uint64_t *pt_entry = &pt[(address >> 12) & 0x1FFU];
        if ((*pt_entry & VMM_PAGE_PRESENT) == 0U) kernel_panic("KERNEL_TEXT_UNMAPPED");
        *pt_entry &= ~VMM_PAGE_NX;
    }
}

bool vm_map_page(uint64_t virtual_address, uint64_t physical_address, uint64_t flags) {
    return vm_map_page_in(vmm_active_pml4, virtual_address, physical_address, flags);
}

bool vm_map_page_in(uint64_t pml4_physical_address, uint64_t virtual_address,
                    uint64_t physical_address, uint64_t flags) {
    if (pml4_physical_address == 0U) {
        return false;
    }
    if (!vmm_is_page_aligned(virtual_address) || !vmm_is_page_aligned(physical_address)) {
        return false;
    }
    if (!vmm_is_canonical(virtual_address)) {
        return false;
    }
    bool user = (flags & VMM_PAGE_USER) != 0U;
    if (user && (virtual_address >= 0x0000800000000000ULL ||
                 ((virtual_address >> 39) & 0x1FFU) >= 256U)) {
        return false;
    }

    uint64_t *pml4 = (uint64_t *)vmm_phys_to_virt(pml4_physical_address);
    uint64_t pml4_index = (virtual_address >> 39) & 0x1FFU;
    uint64_t pdpt_index = (virtual_address >> 30) & 0x1FFU;
    uint64_t pd_index   = (virtual_address >> 21) & 0x1FFU;
    uint64_t pt_index   = (virtual_address >> 12) & 0x1FFU;

    uint64_t *created_entries[3];
    uint64_t created_pages[3];
    size_t created_count = 0;
    uint64_t created_page = 0;
    uint64_t *pdpt = vmm_get_or_create_table(pml4, pml4_index, &created_page);
    if (pdpt == NULL) {
        return false;
    }
    if (created_page != 0U) {
        created_entries[created_count] = &pml4[pml4_index];
        created_pages[created_count++] = created_page;
    }
    uint64_t *pd = vmm_get_or_create_table(pdpt, pdpt_index, &created_page);
    if (pd == NULL) {
        vmm_rollback_tables(created_entries, created_pages, created_count);
        return false;
    }
    if (created_page != 0U) {
        created_entries[created_count] = &pdpt[pdpt_index];
        created_pages[created_count++] = created_page;
    }
    uint64_t *pt = vmm_get_or_create_table(pd, pd_index, &created_page);
    if (pt == NULL) {
        vmm_rollback_tables(created_entries, created_pages, created_count);
        return false;
    }
    if (created_page != 0U) {
        created_entries[created_count] = &pd[pd_index];
        created_pages[created_count++] = created_page;
    }

    /* Don't silently overwrite a mapping that's already there. */
    if ((pt[pt_index] & VMM_PAGE_PRESENT) != 0U) {
        vmm_rollback_tables(created_entries, created_pages, created_count);
        return false;
    }

    if (user) {
        pml4[pml4_index] |= VMM_PAGE_USER | VMM_PAGE_WRITABLE;
        pdpt[pdpt_index] |= VMM_PAGE_USER | VMM_PAGE_WRITABLE;
        pd[pd_index] |= VMM_PAGE_USER | VMM_PAGE_WRITABLE;
    }
    pt[pt_index] = (physical_address & VMM_PAGE_ADDRESS_MASK) | flags | VMM_PAGE_PRESENT;
    ++vmm_mapped_pages;
    if (pml4_physical_address == vmm_active_pml4) {
        __asm__ volatile ("invlpg (%0)" : : "r"(virtual_address) : "memory");
    }
    return true;
}

bool vm_unmap_page(uint64_t virtual_address) {
    if (vmm_active_pml4 == 0U) {
        return false;
    }
    if (!vmm_is_page_aligned(virtual_address) || !vmm_is_canonical(virtual_address)) {
        return false;
    }

    uint64_t *pml4 = (uint64_t *)vmm_phys_to_virt(vmm_active_pml4);
    uint64_t pml4_index = (virtual_address >> 39) & 0x1FFU;
    uint64_t pdpt_index = (virtual_address >> 30) & 0x1FFU;
    uint64_t pd_index   = (virtual_address >> 21) & 0x1FFU;
    uint64_t pt_index   = (virtual_address >> 12) & 0x1FFU;

    if ((pml4[pml4_index] & VMM_PAGE_PRESENT) == 0U) {
        return false;
    }

    uint64_t *pdpt = (uint64_t *)vmm_phys_to_virt(pml4[pml4_index] & VMM_PAGE_ADDRESS_MASK);
    if ((pdpt[pdpt_index] & VMM_PAGE_PRESENT) == 0U) {
        return false;
    }
    /* Unmapping huge pages isn't supported yet. */
    if ((pdpt[pdpt_index] & VMM_PAGE_LARGE) != 0U) {
        return false;
    }

    uint64_t *pd = (uint64_t *)vmm_phys_to_virt(pdpt[pdpt_index] & VMM_PAGE_ADDRESS_MASK);
    if ((pd[pd_index] & VMM_PAGE_PRESENT) == 0U) {
        return false;
    }
    if ((pd[pd_index] & VMM_PAGE_LARGE) != 0U) {
        return false;
    }

    uint64_t *pt = (uint64_t *)vmm_phys_to_virt(pd[pd_index] & VMM_PAGE_ADDRESS_MASK);
    if ((pt[pt_index] & VMM_PAGE_PRESENT) == 0U) {
        return false;
    }

    pt[pt_index] = 0;
    __asm__ volatile ("invlpg (%0)" : : "r"(virtual_address) : "memory");
    --vmm_mapped_pages;
    return true;
}

bool vm_get_physical(uint64_t virtual_address, uint64_t *physical_address) {
    return vm_get_physical_in(vmm_active_pml4, virtual_address, physical_address);
}

bool vm_get_physical_in(uint64_t pml4_physical_address, uint64_t virtual_address,
                        uint64_t *physical_address) {
    if (physical_address == NULL || pml4_physical_address == 0U) {
        return false;
    }
    if (!vmm_is_canonical(virtual_address)) {
        return false;
    }

    uint64_t *pml4 = (uint64_t *)vmm_phys_to_virt(pml4_physical_address);
    uint64_t pml4_index = (virtual_address >> 39) & 0x1FFU;
    uint64_t pdpt_index = (virtual_address >> 30) & 0x1FFU;
    uint64_t pd_index   = (virtual_address >> 21) & 0x1FFU;
    uint64_t pt_index   = (virtual_address >> 12) & 0x1FFU;

    if ((pml4[pml4_index] & VMM_PAGE_PRESENT) == 0U) {
        return false;
    }

    uint64_t *pdpt = (uint64_t *)vmm_phys_to_virt(pml4[pml4_index] & VMM_PAGE_ADDRESS_MASK);
    if ((pdpt[pdpt_index] & VMM_PAGE_PRESENT) == 0U) {
        return false;
    }
    if ((pdpt[pdpt_index] & VMM_PAGE_LARGE) != 0U) {
        *physical_address = (pdpt[pdpt_index] & 0x000FFFFFC0000000ULL) |
                            (virtual_address & 0x3FFFFFFFULL);
        return true; /* 1 GiB page */
    }

    uint64_t *pd = (uint64_t *)vmm_phys_to_virt(pdpt[pdpt_index] & VMM_PAGE_ADDRESS_MASK);
    if ((pd[pd_index] & VMM_PAGE_PRESENT) == 0U) {
        return false;
    }
    if ((pd[pd_index] & VMM_PAGE_LARGE) != 0U) {
        *physical_address = (pd[pd_index] & 0x000FFFFFFFE00000ULL) |
                            (virtual_address & 0x1FFFFFULL);
        return true; /* 2 MiB page */
    }

    uint64_t *pt = (uint64_t *)vmm_phys_to_virt(pd[pd_index] & VMM_PAGE_ADDRESS_MASK);
    if ((pt[pt_index] & VMM_PAGE_PRESENT) == 0U) {
        return false;
    }

    *physical_address = (pt[pt_index] & VMM_PAGE_ADDRESS_MASK) |
                        (virtual_address & 0xFFFU);
    return true;
}

bool vm_get_page_flags_in(uint64_t pml4_physical_address, uint64_t virtual_address,
                          uint64_t *flags) {
    if (flags == NULL || pml4_physical_address == 0U ||
        !vmm_is_canonical(virtual_address)) return false;

    uint64_t *pml4 = (uint64_t *)vmm_phys_to_virt(pml4_physical_address);
    uint64_t entry = pml4[(virtual_address >> 39) & 0x1FFU];
    if ((entry & VMM_PAGE_PRESENT) == 0U) return false;
    uint64_t *pdpt = (uint64_t *)vmm_phys_to_virt(entry & VMM_PAGE_ADDRESS_MASK);

    entry = pdpt[(virtual_address >> 30) & 0x1FFU];
    if ((entry & VMM_PAGE_PRESENT) == 0U || (entry & VMM_PAGE_LARGE) != 0U) return false;
    uint64_t *pd = (uint64_t *)vmm_phys_to_virt(entry & VMM_PAGE_ADDRESS_MASK);

    entry = pd[(virtual_address >> 21) & 0x1FFU];
    if ((entry & VMM_PAGE_PRESENT) == 0U || (entry & VMM_PAGE_LARGE) != 0U) return false;
    uint64_t *pt = (uint64_t *)vmm_phys_to_virt(entry & VMM_PAGE_ADDRESS_MASK);

    entry = pt[(virtual_address >> 12) & 0x1FFU];
    if ((entry & VMM_PAGE_PRESENT) == 0U) return false;
    *flags = entry & (0xFFFULL | VMM_PAGE_NX);
    return true;
}

static bool vm_user_page_accessible(uint64_t pml4_physical_address, uint64_t address,
                                    bool require_write) {
    uint64_t required = VMM_PAGE_PRESENT | VMM_PAGE_USER;
    if (require_write) required |= VMM_PAGE_WRITABLE;
    uint64_t *pml4 = (uint64_t *)vmm_phys_to_virt(pml4_physical_address);
    uint64_t pml4_entry = pml4[(address >> 39) & 0x1FFU];
    if ((pml4_entry & required) != required) return false;
    uint64_t *pdpt = (uint64_t *)vmm_phys_to_virt(pml4_entry & VMM_PAGE_ADDRESS_MASK);
    uint64_t pdpt_entry = pdpt[(address >> 30) & 0x1FFU];
    if ((pdpt_entry & required) != required || (pdpt_entry & VMM_PAGE_LARGE)) return false;
    uint64_t *pd = (uint64_t *)vmm_phys_to_virt(pdpt_entry & VMM_PAGE_ADDRESS_MASK);
    uint64_t pd_entry = pd[(address >> 21) & 0x1FFU];
    if ((pd_entry & required) != required || (pd_entry & VMM_PAGE_LARGE)) return false;
    uint64_t *pt = (uint64_t *)vmm_phys_to_virt(pd_entry & VMM_PAGE_ADDRESS_MASK);
    uint64_t pt_entry = pt[(address >> 12) & 0x1FFU];
    return (pt_entry & required) == required;
}

static bool vm_user_range_access_valid(uint64_t pml4_physical_address,
                                       uint64_t address, uint64_t length,
                                       bool require_write) {
    const uint64_t user_limit = 0x0000800000000000ULL;
    if (pml4_physical_address == 0U || address >= user_limit ||
        length > user_limit - address) return false;
    if (length == 0U) return true;
    uint64_t last = address + length - 1U;
    for (uint64_t page = address & ~0xFFFULL; page <= (last & ~0xFFFULL); page += VMM_PAGE_SIZE) {
        if (!vm_user_page_accessible(pml4_physical_address, page, require_write)) return false;
        if (page > UINT64_MAX - VMM_PAGE_SIZE) return false;
    }
    return true;
}

bool vm_user_range_valid(uint64_t pml4_physical_address, uint64_t address, uint64_t length) {
    return vm_user_range_access_valid(pml4_physical_address, address, length, false);
}

bool vm_user_range_writable(uint64_t pml4_physical_address, uint64_t address,
                            uint64_t length) {
    return vm_user_range_access_valid(pml4_physical_address, address, length, true);
}

static bool vm_destroy_lower_table(uint64_t physical_address, unsigned int level) {
    uint64_t *table = (uint64_t *)vmm_phys_to_virt(physical_address);
    for (size_t i = 0; i < PAGE_TABLE_ENTRIES; ++i) {
        uint64_t entry = table[i];
        if ((entry & VMM_PAGE_PRESENT) == 0U) continue;
        if (level > 1U) {
            if ((entry & VMM_PAGE_LARGE) != 0U) return false;
            uint64_t child = entry & VMM_PAGE_ADDRESS_MASK;
            if (!vm_destroy_lower_table(child, level - 1U)) return false;
            pmm_free_page(child);
            --vmm_page_table_count;
        } else {
            pmm_free_page(entry & VMM_PAGE_ADDRESS_MASK);
            --vmm_mapped_pages;
        }
        table[i] = 0;
    }
    return true;
}

bool vm_destroy_address_space(uint64_t pml4_physical_address) {
    if (pml4_physical_address == 0U || pml4_physical_address == vmm_active_pml4 ||
        !vmm_is_page_aligned(pml4_physical_address)) return false;
    uint64_t *pml4 = (uint64_t *)vmm_phys_to_virt(pml4_physical_address);
    for (size_t i = 0; i < 256U; ++i) {
        uint64_t entry = pml4[i];
        if ((entry & VMM_PAGE_PRESENT) == 0U) continue;
        if ((entry & VMM_PAGE_LARGE) != 0U) return false;
        uint64_t pdpt = entry & VMM_PAGE_ADDRESS_MASK;
        if (!vm_destroy_lower_table(pdpt, 3U)) return false;
        pmm_free_page(pdpt);
        --vmm_page_table_count;
        pml4[i] = 0;
    }
    pmm_free_page(pml4_physical_address);
    return true;
}

bool vm_is_mapped(uint64_t virtual_address) {
    if (vmm_active_pml4 == 0U || !vmm_is_canonical(virtual_address)) {
        return false;
    }

    uint64_t *pml4 = (uint64_t *)vmm_phys_to_virt(vmm_active_pml4);
    uint64_t pml4_index = (virtual_address >> 39) & 0x1FFU;
    uint64_t pdpt_index = (virtual_address >> 30) & 0x1FFU;
    uint64_t pd_index   = (virtual_address >> 21) & 0x1FFU;
    uint64_t pt_index   = (virtual_address >> 12) & 0x1FFU;

    if ((pml4[pml4_index] & VMM_PAGE_PRESENT) == 0U) {
        return false;
    }

    uint64_t *pdpt = (uint64_t *)vmm_phys_to_virt(pml4[pml4_index] & VMM_PAGE_ADDRESS_MASK);
    if ((pdpt[pdpt_index] & VMM_PAGE_PRESENT) == 0U) {
        return false;
    }
    if ((pdpt[pdpt_index] & VMM_PAGE_LARGE) != 0U) {
        return true; /* 1 GiB page */
    }

    uint64_t *pd = (uint64_t *)vmm_phys_to_virt(pdpt[pdpt_index] & VMM_PAGE_ADDRESS_MASK);
    if ((pd[pd_index] & VMM_PAGE_PRESENT) == 0U) {
        return false;
    }
    if ((pd[pd_index] & VMM_PAGE_LARGE) != 0U) {
        return true; /* 2 MiB page */
    }

    uint64_t *pt = (uint64_t *)vmm_phys_to_virt(pd[pd_index] & VMM_PAGE_ADDRESS_MASK);
    return (pt[pt_index] & VMM_PAGE_PRESENT) != 0U;
}

uint64_t vm_get_mapped_pages(void) {
    return vmm_mapped_pages;
}

uint64_t vm_phys_to_virt(uint64_t physical_address) {
    return physical_address + vmm_hhdm_offset;
}

uint64_t vm_get_page_table_count(void) {
    return vmm_page_table_count;
}

void vm_init(void) {
    if (vmm_initialized) {
        return;
    }

    vmm_serial_write("VMM: vm_init start\n");

    if (limine_hhdm_request.response == NULL) {
        kernel_panic("PAGE_TABLE_INIT_NO_HHDM");
    }

    vmm_hhdm_offset = limine_hhdm_request.response->offset;
    vmm_serial_write("VMM: hhdm offset: "); vmm_serial_hex64(vmm_hhdm_offset);

    /* The kernel is already running on Limine's page tables. We build our
     * own PML4 and copy the bootloader's top-level entries into it, so
     * every mapping the kernel needs (code, HHDM, stack, IDT, framebuffer)
     * survives the switch. After this, CR3 points at our page tables. */
    vmm_active_pml4 = vm_create_address_space();
    if (vmm_active_pml4 == 0U) {
        kernel_panic("PAGE_TABLE_ALLOC_FAILED");
    }
    vmm_serial_write("VMM: our pml4 phys: "); vmm_serial_hex64(vmm_active_pml4);

    uint64_t boot_cr3 = 0;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(boot_cr3));
    vmm_serial_write("VMM: limine cr3 phys: "); vmm_serial_hex64(boot_cr3);

    if (boot_cr3 == 0U || !vmm_is_page_aligned(boot_cr3)) {
        kernel_panic("INVALID_PAGE_TABLE");
    }

    uint64_t *new_pml4 = (uint64_t *)vmm_phys_to_virt(vmm_active_pml4);
    uint64_t *boot_pml4 = (uint64_t *)vmm_phys_to_virt(boot_cr3);
    for (size_t i = 0; i < PAGE_TABLE_ENTRIES; ++i) {
        new_pml4[i] = boot_pml4[i];
    }

    /* The bootloader may use huge pages, so recount walks the real tables
     * to get accurate statistics instead of guessing. */
    vmm_recount();
    vmm_serial_write("VMM: mapped pages: "); vmm_serial_hex64(vmm_mapped_pages);
    vmm_serial_write("VMM: table count:  "); vmm_serial_hex64(vmm_page_table_count);

    /* Sanity checks: everything required to keep executing must be mapped. */
    if (!vm_is_mapped((uint64_t)__kernel_start)) {
        kernel_panic("PAGE_TABLE_KERNEL_UNMAPPED");
    }
    if (!vm_is_mapped(vmm_hhdm_offset)) {
        kernel_panic("PAGE_TABLE_HHDM_UNMAPPED");
    }

    uint64_t rsp_val = 0;
    __asm__ volatile ("mov %%rsp, %0" : "=r"(rsp_val));
    if (!vm_is_mapped(rsp_val)) {
        kernel_panic("PAGE_TABLE_STACK_UNMAPPED");
    }

    vmm_make_kernel_text_executable(vmm_active_pml4);

    vmm_serial_write("VMM: switching to our address space\n");
    __asm__ volatile ("cli");
    vm_switch_address_space(vmm_active_pml4);
    arch_enable_nx();
    __asm__ volatile ("sti");

    if (vm_get_cr3() != vmm_active_pml4) {
        kernel_panic("PAGE_TABLE_CR3_MISMATCH");
    }

    vmm_initialized = true;
    vmm_serial_write("VMM: vm_init done\n");
}

void vm_self_test(void) {
    if (!vmm_initialized) {
        kernel_panic("PAGING_SELF_TEST_NOT_INITIALIZED");
    }

    vmm_serial_write("VMM: self-test start\n");

    uint64_t physical_page = pmm_alloc_page();
    if (physical_page == 0U) {
        kernel_panic("PAGING_SELF_TEST_ALLOC_FAILED");
    }
    if (!vmm_is_page_aligned(physical_page)) {
        kernel_panic("PAGING_SELF_TEST_UNALIGNED");
    }

    /* Pick a fixed test address that's canonical, below the kernel image,
     * and guaranteed not to be mapped by Limine or MyOS. */
    uint64_t virtual_address = 0xffffff8000000000ULL;
    if (vm_is_mapped(virtual_address)) {
        kernel_panic("PAGING_SELF_TEST_VA_IN_USE");
    }

    if (!vm_map_page(virtual_address, physical_page, VMM_PAGE_PRESENT | VMM_PAGE_WRITABLE)) {
        kernel_panic("PAGING_SELF_TEST_MAP_FAILED");
    }

    if (!vm_is_mapped(virtual_address)) {
        kernel_panic("PAGING_SELF_TEST_NOT_MAPPED");
    }

    uint64_t translated = 0;
    if (!vm_get_physical(virtual_address, &translated)) {
        kernel_panic("PAGING_SELF_TEST_TRANSLATE_FAILED");
    }
    if (translated != physical_page) {
        kernel_panic("PAGING_SELF_TEST_TRANSLATION_MISMATCH");
    }

    /* Read/write through the live mapping. */
    volatile uint64_t *mapped = (volatile uint64_t *)virtual_address;
    *mapped = 0xDEADBEEFCAFEBABEULL;
    if (*mapped != 0xDEADBEEFCAFEBABEULL) {
        kernel_panic("PAGING_SELF_TEST_RW_FAILED");
    }

    /* Double-mapping the same virtual page must fail. */
    if (vm_map_page(virtual_address, physical_page, VMM_PAGE_PRESENT | VMM_PAGE_WRITABLE)) {
        kernel_panic("PAGING_SELF_TEST_DOUBLE_MAP_ALLOWED");
    }

    /* Remember PMM state before unmapping. Page-table pages stay allocated
     * (the VMM owns them), but the data page must come back when freed. */
    uint64_t free_before_unmap = pmm_get_free_pages();
    uint64_t used_before_unmap = pmm_get_used_pages();

    if (!vm_unmap_page(virtual_address)) {
        kernel_panic("PAGING_SELF_TEST_UNMAP_FAILED");
    }

    if (vm_is_mapped(virtual_address)) {
        kernel_panic("PAGING_SELF_TEST_STILL_MAPPED");
    }

    if (vm_get_physical(virtual_address, &translated)) {
        kernel_panic("PAGING_SELF_TEST_TRANSLATE_AFTER_UNMAP");
    }

    /* Unmapping itself must not touch PMM accounting. */
    if (pmm_get_free_pages() != free_before_unmap || pmm_get_used_pages() != used_before_unmap) {
        kernel_panic("PAGING_SELF_TEST_UNMAP_DRIFT");
    }

    pmm_free_page(physical_page);

    /* After unmapping and freeing, the data page must be available again.
     * The page-table pages stay allocated on purpose. */
    if (pmm_get_free_pages() != (free_before_unmap + 1U) ||
        pmm_get_used_pages() != (used_before_unmap - 1U)) {
        kernel_panic("PAGING_SELF_TEST_PMM_DRIFT");
    }

    uint64_t user_free_before = pmm_get_free_pages();
    uint64_t user_mapped_before = vmm_mapped_pages;
    uint64_t user_tables_before = vmm_page_table_count;
    uint64_t user_space = vm_create_address_space();
    uint64_t user_page = pmm_alloc_page();
    if (user_space == 0U || user_page == 0U ||
        !vm_map_page_in(user_space, 0x00400000ULL, user_page,
                        VMM_PAGE_USER | VMM_PAGE_WRITABLE)) {
        if (user_page != 0U) pmm_free_page(user_page);
        (void)vm_destroy_address_space(user_space);
        kernel_panic("PAGING_SELF_TEST_USER_MAP_FAILED");
    }
    uint64_t *user_pml4 = (uint64_t *)vmm_phys_to_virt(user_space);
    uint64_t kernel_pml4_index = ((uint64_t)__kernel_start >> 39) & 0x1FFU;
    if ((user_pml4[kernel_pml4_index] & VMM_PAGE_USER) != 0U ||
        !vm_user_range_valid(user_space, 0x00400000ULL, 1U) ||
        vm_user_range_valid(user_space, (uint64_t)__kernel_start, 1U) ||
        vm_map_page_in(user_space, (uint64_t)__kernel_start, user_page, VMM_PAGE_USER)) {
        (void)vm_destroy_address_space(user_space);
        kernel_panic("PAGING_SELF_TEST_USER_ISOLATION_FAILED");
    }
    uint64_t supervisor_page = pmm_alloc_page();
    if (supervisor_page == 0U) {
        (void)vm_destroy_address_space(user_space);
        kernel_panic("PAGING_SELF_TEST_SUPERVISOR_PAGE_FAILED");
    }
    if (!vm_map_page_in(user_space, 0x00500000ULL, supervisor_page, VMM_PAGE_WRITABLE)) {
        pmm_free_page(supervisor_page);
        (void)vm_destroy_address_space(user_space);
        kernel_panic("PAGING_SELF_TEST_SUPERVISOR_PAGE_FAILED");
    }
    if (vm_user_range_valid(user_space, 0x00500000ULL, 1U)) {
        (void)vm_destroy_address_space(user_space);
        kernel_panic("PAGING_SELF_TEST_SUPERVISOR_PAGE_FAILED");
    }
    if (!vm_destroy_address_space(user_space)) {
        kernel_panic("PAGING_SELF_TEST_USER_SPACE_DESTROY_FAILED");
    }
    if (pmm_get_free_pages() != user_free_before ||
        vmm_mapped_pages != user_mapped_before ||
        vmm_page_table_count != user_tables_before) {
        kernel_panic("PAGING_SELF_TEST_USER_RECLAIM_FAILED");
    }

    vmm_serial_write("VMM: self-test passed\n");
}