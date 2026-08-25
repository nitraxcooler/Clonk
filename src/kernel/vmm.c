#include "kernel/vmm.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "boot/limine.h"
#include "kernel/panic.h"
#include "kernel/pmm.h"
#include "arch/x86_64/io.h"

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
static uint64_t *vmm_get_or_create_table(uint64_t *parent_table, uint64_t index) {
    uint64_t entry = parent_table[index];

    if ((entry & VMM_PAGE_PRESENT) != 0U) {
        if ((entry & VMM_PAGE_LARGE) != 0U) {
            return NULL; /* it's a huge page — nothing to descend into */
        }
        return (uint64_t *)vmm_phys_to_virt(entry & ~0xFFFULL);
    }

    uint64_t physical_address = pmm_alloc_page();
    if (physical_address == 0U) {
        kernel_panic("PAGE_TABLE_ALLOC_FAILED");
    }
    if (!vmm_is_page_aligned(physical_address)) {
        kernel_panic("PAGE_TABLE_NOT_ALIGNED");
    }

    uint64_t *table = (uint64_t *)vmm_phys_to_virt(physical_address);
    vmm_zero_memory((uint8_t *)table, VMM_PAGE_SIZE);
    parent_table[index] = physical_address | VMM_PAGE_PRESENT | VMM_PAGE_WRITABLE;
    ++vmm_page_table_count;
    return table;
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

        uint64_t *pdpt = (uint64_t *)vmm_phys_to_virt(pml4_entry & ~0xFFFULL);
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

            uint64_t *pd = (uint64_t *)vmm_phys_to_virt(pdpt_entry & ~0xFFFULL);
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

                uint64_t *pt = (uint64_t *)vmm_phys_to_virt(pd_entry & ~0xFFFULL);
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
        kernel_panic("PAGE_TABLE_ALLOC_FAILED");
    }
    if (!vmm_is_page_aligned(pml4_physical)) {
        kernel_panic("PAGE_TABLE_NOT_ALIGNED");
    }

    uint64_t *pml4 = (uint64_t *)vmm_phys_to_virt(pml4_physical);
    vmm_zero_memory((uint8_t *)pml4, VMM_PAGE_SIZE);
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

bool vm_map_page(uint64_t virtual_address, uint64_t physical_address, uint64_t flags) {
    if (vmm_active_pml4 == 0U) {
        return false;
    }
    if (!vmm_is_page_aligned(virtual_address) || !vmm_is_page_aligned(physical_address)) {
        return false;
    }
    if (!vmm_is_canonical(virtual_address)) {
        return false;
    }

    uint64_t *pml4 = (uint64_t *)vmm_phys_to_virt(vmm_active_pml4);
    uint64_t pml4_index = (virtual_address >> 39) & 0x1FFU;
    uint64_t pdpt_index = (virtual_address >> 30) & 0x1FFU;
    uint64_t pd_index   = (virtual_address >> 21) & 0x1FFU;
    uint64_t pt_index   = (virtual_address >> 12) & 0x1FFU;

    uint64_t *pdpt = vmm_get_or_create_table(pml4, pml4_index);
    if (pdpt == NULL) {
        return false;
    }
    uint64_t *pd = vmm_get_or_create_table(pdpt, pdpt_index);
    if (pd == NULL) {
        return false;
    }
    uint64_t *pt = vmm_get_or_create_table(pd, pd_index);
    if (pt == NULL) {
        return false;
    }

    /* Don't silently overwrite a mapping that's already there. */
    if ((pt[pt_index] & VMM_PAGE_PRESENT) != 0U) {
        return false;
    }

    pt[pt_index] = (physical_address & ~0xFFFULL) | flags | VMM_PAGE_PRESENT;
    ++vmm_mapped_pages;
    __asm__ volatile ("invlpg (%0)" : : "r"(virtual_address) : "memory");
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

    uint64_t *pdpt = (uint64_t *)vmm_phys_to_virt(pml4[pml4_index] & ~0xFFFULL);
    if ((pdpt[pdpt_index] & VMM_PAGE_PRESENT) == 0U) {
        return false;
    }
    /* Unmapping huge pages isn't supported yet. */
    if ((pdpt[pdpt_index] & VMM_PAGE_LARGE) != 0U) {
        return false;
    }

    uint64_t *pd = (uint64_t *)vmm_phys_to_virt(pdpt[pdpt_index] & ~0xFFFULL);
    if ((pd[pd_index] & VMM_PAGE_PRESENT) == 0U) {
        return false;
    }
    if ((pd[pd_index] & VMM_PAGE_LARGE) != 0U) {
        return false;
    }

    uint64_t *pt = (uint64_t *)vmm_phys_to_virt(pd[pd_index] & ~0xFFFULL);
    if ((pt[pt_index] & VMM_PAGE_PRESENT) == 0U) {
        return false;
    }

    pt[pt_index] = 0;
    __asm__ volatile ("invlpg (%0)" : : "r"(virtual_address) : "memory");
    --vmm_mapped_pages;
    return true;
}

bool vm_get_physical(uint64_t virtual_address, uint64_t *physical_address) {
    if (physical_address == NULL || vmm_active_pml4 == 0U) {
        return false;
    }
    if (!vmm_is_canonical(virtual_address)) {
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

    uint64_t *pdpt = (uint64_t *)vmm_phys_to_virt(pml4[pml4_index] & ~0xFFFULL);
    if ((pdpt[pdpt_index] & VMM_PAGE_PRESENT) == 0U) {
        return false;
    }
    if ((pdpt[pdpt_index] & VMM_PAGE_LARGE) != 0U) {
        *physical_address = (pdpt[pdpt_index] & ~0x3FFFFFFFULL) | (virtual_address & 0x3FFFFFFFULL);
        return true; /* 1 GiB page */
    }

    uint64_t *pd = (uint64_t *)vmm_phys_to_virt(pdpt[pdpt_index] & ~0xFFFULL);
    if ((pd[pd_index] & VMM_PAGE_PRESENT) == 0U) {
        return false;
    }
    if ((pd[pd_index] & VMM_PAGE_LARGE) != 0U) {
        *physical_address = (pd[pd_index] & ~0x1FFFFFULL) | (virtual_address & 0x1FFFFFULL);
        return true; /* 2 MiB page */
    }

    uint64_t *pt = (uint64_t *)vmm_phys_to_virt(pd[pd_index] & ~0xFFFULL);
    if ((pt[pt_index] & VMM_PAGE_PRESENT) == 0U) {
        return false;
    }

    *physical_address = (pt[pt_index] & ~0xFFFULL) | (virtual_address & 0xFFFU);
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

    uint64_t *pdpt = (uint64_t *)vmm_phys_to_virt(pml4[pml4_index] & ~0xFFFULL);
    if ((pdpt[pdpt_index] & VMM_PAGE_PRESENT) == 0U) {
        return false;
    }
    if ((pdpt[pdpt_index] & VMM_PAGE_LARGE) != 0U) {
        return true; /* 1 GiB page */
    }

    uint64_t *pd = (uint64_t *)vmm_phys_to_virt(pdpt[pdpt_index] & ~0xFFFULL);
    if ((pd[pd_index] & VMM_PAGE_PRESENT) == 0U) {
        return false;
    }
    if ((pd[pd_index] & VMM_PAGE_LARGE) != 0U) {
        return true; /* 2 MiB page */
    }

    uint64_t *pt = (uint64_t *)vmm_phys_to_virt(pd[pd_index] & ~0xFFFULL);
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

    vmm_serial_write("VMM: switching to our address space\n");
    __asm__ volatile ("cli");
    vm_switch_address_space(vmm_active_pml4);
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

    vmm_serial_write("VMM: self-test passed\n");
}