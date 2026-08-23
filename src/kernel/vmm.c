#include "kernel/vmm.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "boot/limine.h"
#include "kernel/panic.h"
#include "kernel/pmm.h"
#include "arch/x86_64/io.h"

/* Serial helpers for VMM debugging (COM1) */
static inline void vmm_serial_putc(char c) { outb(0x3F8, (uint8_t)c); }
static void vmm_serial_write(const char *s) { while (*s) vmm_serial_putc(*s++); }
static void vmm_serial_hex64(uint64_t v) {
    const char *digits = "0123456789ABCDEF";
    vmm_serial_write("0x");
    for (int i = 15; i >= 0; --i) {
        uint8_t nibble = (v >> (i * 4)) & 0xF;
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

static void vmm_zero_memory(uint8_t *ptr, size_t length) {
    for (size_t i = 0; i < length; ++i) {
        ptr[i] = 0;
    }
}

static uint64_t vmm_phys_to_virt(uint64_t physical_address) {
    return physical_address + vmm_hhdm_offset;
}

static uint64_t vmm_virt_to_phys(uint64_t virtual_address) {
    return virtual_address - vmm_hhdm_offset;
}

static uint64_t *vmm_allocate_table(void) {
    uint64_t physical_address = pmm_alloc_page();
    if (physical_address == 0U) {
        kernel_panic("VMM page-table allocation failed");
    }

    uint64_t *table = (uint64_t *)vmm_phys_to_virt(physical_address);
    vmm_zero_memory((uint8_t *)table, VMM_PAGE_SIZE);
    ++vmm_page_table_count;
    return table;
}

static uint64_t *vmm_get_or_create_table(uint64_t *parent_table, uint64_t index, bool *created) {
    uint64_t entry = parent_table[index];
    if ((entry & VMM_PAGE_PRESENT) == 0U) {
        uint64_t page = pmm_alloc_page();
        if (page == 0U) {
            kernel_panic("VMM table allocation failed");
        }

        uint64_t *table = (uint64_t *)vmm_phys_to_virt(page);
        vmm_zero_memory((uint8_t *)table, VMM_PAGE_SIZE);
        parent_table[index] = page | VMM_PAGE_PRESENT | VMM_PAGE_WRITABLE;
        ++vmm_page_table_count;
        if (created != NULL) {
            *created = true;
        }
        return table;
    }

    if (created != NULL) {
        *created = false;
    }
    return (uint64_t *)vmm_phys_to_virt(entry & ~0xFFFULL);
}

static uint64_t vmm_align_up(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1U) & ~(alignment - 1U);
}

static uint64_t vmm_align_down(uint64_t value, uint64_t alignment) {
    return value & ~(alignment - 1U);
}

uint64_t vm_create_address_space(void) {
    uint64_t pml4_physical = pmm_alloc_page();
    if (pml4_physical == 0U) {
        kernel_panic("VMM: failed to allocate PML4");
    }

    uint64_t *pml4 = (uint64_t *)vmm_phys_to_virt(pml4_physical);
    vmm_zero_memory((uint8_t *)pml4, VMM_PAGE_SIZE);
    return pml4_physical;
}

void vm_switch_address_space(uint64_t pml4_physical_address) {
    vmm_active_pml4 = pml4_physical_address;
    __asm__ volatile ("mov %0, %%cr3" : : "r"(pml4_physical_address) : "memory");
    __asm__ volatile ("mov %%cr3, %%rax\n\tmov %%rax, %%cr3" : : : "rax");
}

uint64_t vm_get_active_pml4(void) {
    return vmm_active_pml4;
}

bool vm_is_initialized(void) {
    return vmm_initialized;
}

bool vm_map_page(uint64_t virtual_address, uint64_t physical_address, uint64_t flags) {
    if (vmm_active_pml4 == 0U) {
        return false;
    }

    uint64_t *pml4 = (uint64_t *)vmm_phys_to_virt(vmm_active_pml4);
    uint64_t pml4_index = (virtual_address >> 39) & 0x1FFU;
    uint64_t pdpt_index = (virtual_address >> 30) & 0x1FFU;
    uint64_t pd_index = (virtual_address >> 21) & 0x1FFU;
    uint64_t pt_index = (virtual_address >> 12) & 0x1FFU;

    bool created = false;
    uint64_t *pdpt = vmm_get_or_create_table(pml4, pml4_index, &created);
    (void)created;

    uint64_t *pd = vmm_get_or_create_table(pdpt, pdpt_index, &created);
    (void)created;

    uint64_t *pt = vmm_get_or_create_table(pd, pd_index, &created);
    (void)created;

    if ((pt[pt_index] & VMM_PAGE_PRESENT) != 0U) {
        pt[pt_index] = (physical_address & ~0xFFFULL) | flags | VMM_PAGE_PRESENT;
        return true;
    }

    pt[pt_index] = (physical_address & ~0xFFFULL) | flags | VMM_PAGE_PRESENT;
    ++vmm_mapped_pages;
    return true;
}

bool vm_unmap_page(uint64_t virtual_address) {
    if (vmm_active_pml4 == 0U) {
        return false;
    }

    uint64_t *pml4 = (uint64_t *)vmm_phys_to_virt(vmm_active_pml4);
    uint64_t pml4_index = (virtual_address >> 39) & 0x1FFU;
    uint64_t pdpt_index = (virtual_address >> 30) & 0x1FFU;
    uint64_t pd_index = (virtual_address >> 21) & 0x1FFU;
    uint64_t pt_index = (virtual_address >> 12) & 0x1FFU;

    if ((pml4[pml4_index] & VMM_PAGE_PRESENT) == 0U) {
        return false;
    }

    uint64_t *pdpt = (uint64_t *)vmm_phys_to_virt(pml4[pml4_index] & ~0xFFFULL);
    if ((pdpt[pdpt_index] & VMM_PAGE_PRESENT) == 0U) {
        return false;
    }

    uint64_t *pd = (uint64_t *)vmm_phys_to_virt(pdpt[pdpt_index] & ~0xFFFULL);
    if ((pd[pd_index] & VMM_PAGE_PRESENT) == 0U) {
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

    uint64_t *pml4 = (uint64_t *)vmm_phys_to_virt(vmm_active_pml4);
    uint64_t pml4_index = (virtual_address >> 39) & 0x1FFU;
    uint64_t pdpt_index = (virtual_address >> 30) & 0x1FFU;
    uint64_t pd_index = (virtual_address >> 21) & 0x1FFU;
    uint64_t pt_index = (virtual_address >> 12) & 0x1FFU;

    if ((pml4[pml4_index] & VMM_PAGE_PRESENT) == 0U) {
        return false;
    }

    uint64_t *pdpt = (uint64_t *)vmm_phys_to_virt(pml4[pml4_index] & ~0xFFFULL);
    if ((pdpt[pdpt_index] & VMM_PAGE_PRESENT) == 0U) {
        return false;
    }

    uint64_t *pd = (uint64_t *)vmm_phys_to_virt(pdpt[pdpt_index] & ~0xFFFULL);
    if ((pd[pd_index] & VMM_PAGE_PRESENT) == 0U) {
        return false;
    }

    uint64_t *pt = (uint64_t *)vmm_phys_to_virt(pd[pd_index] & ~0xFFFULL);
    if ((pt[pt_index] & VMM_PAGE_PRESENT) == 0U) {
        return false;
    }

    *physical_address = (pt[pt_index] & ~0xFFFULL) | (virtual_address & 0xFFFU);
    return true;
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

static void vmm_map_kernel_range(uint64_t virtual_start, uint64_t virtual_end) {
    uint64_t start = vmm_align_down(virtual_start, VMM_PAGE_SIZE);
    uint64_t end = vmm_align_up(virtual_end, VMM_PAGE_SIZE);

    for (uint64_t virtual_address = start; virtual_address < end; virtual_address += VMM_PAGE_SIZE) {
        uint64_t physical_address = virtual_address - vmm_hhdm_offset;
        vm_map_page(virtual_address, physical_address, VMM_PAGE_PRESENT | VMM_PAGE_WRITABLE);
    }
}

static void vmm_map_hhdm_for_usable_memory(void) {
    if (limine_memmap_request.response == NULL) {
        kernel_panic("VMM memory map unavailable");
    }

    struct limine_memmap_entry **entries = limine_memmap_request.response->entries;
    uint64_t count = limine_memmap_request.response->entry_count;

    for (uint64_t i = 0; i < count; ++i) {
        struct limine_memmap_entry *entry = entries[i];
        if (entry == NULL) {
            continue;
        }

        if (entry->type != LIMINE_MEMMAP_USABLE) {
            continue;
        }

        uint64_t base = vmm_align_up(entry->base, VMM_PAGE_SIZE);
        uint64_t end = vmm_align_down(entry->base + entry->length, VMM_PAGE_SIZE);
        for (uint64_t physical_address = base; physical_address < end; physical_address += VMM_PAGE_SIZE) {
            uint64_t virtual_address = physical_address + vmm_hhdm_offset;
            vm_map_page(virtual_address, physical_address, VMM_PAGE_PRESENT | VMM_PAGE_WRITABLE);
        }
    }
}

void vm_init(void) {
    if (vmm_initialized) {
        return;
    }

    vmm_serial_write("VMM: vm_init start\n");

    if (limine_hhdm_request.response == NULL) {
        kernel_panic("VMM HHDM response missing");
    }

    vmm_hhdm_offset = limine_hhdm_request.response->offset;
    vmm_serial_write("VMM: HHDM offset: "); vmm_serial_hex64(vmm_hhdm_offset);

    vmm_active_pml4 = vm_create_address_space();
    vmm_serial_write("VMM: pml4 phys: "); vmm_serial_hex64(vmm_active_pml4);

    vmm_initialized = true;

    uint64_t kernel_start = (uint64_t)__kernel_start;
    uint64_t kernel_end = (uint64_t)__kernel_end;
    if (kernel_start == 0 || kernel_end <= kernel_start) {
        kernel_panic("VMM kernel bounds invalid");
    }

    vmm_serial_write("VMM: mapping kernel range\n");
    vmm_map_kernel_range(kernel_start, kernel_end);
    vmm_serial_write("VMM: mapping HHDM usable ranges\n");
    vmm_map_hhdm_for_usable_memory();

    vmm_serial_write("VMM: mapping identity 0-2MB\n");
    for (uint64_t address = 0; address < 2ULL * 1024ULL * 1024ULL; address += VMM_PAGE_SIZE) {
        vm_map_page(address, address, VMM_PAGE_PRESENT | VMM_PAGE_WRITABLE);
    }

    /* Ensure the current stack page(s) are mapped in the new PML4 before switching CR3 */
    uint64_t rsp_val = 0;
    __asm__ volatile ("mov %%rsp, %0" : "=r"(rsp_val));
    uint64_t rsp_page = vmm_align_down(rsp_val, VMM_PAGE_SIZE);
    uint64_t rsp_phys = vmm_virt_to_phys(rsp_page);
    vmm_serial_write("VMM: current RSP: "); vmm_serial_hex64(rsp_val);
    vmm_serial_write("VMM: mapping stack page for RSP\n");
    vm_map_page(rsp_page, rsp_phys, VMM_PAGE_PRESENT | VMM_PAGE_WRITABLE);

    /* Copy current active PML4 into the new PML4 so all existing mappings remain available after CR3 switch */
    uint64_t old_cr3 = 0;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(old_cr3));
    vmm_serial_write("VMM: current CR3 phys: "); vmm_serial_hex64(old_cr3);
    void *old_pml4 = (void *)vmm_phys_to_virt(old_cr3);
    void *new_pml4 = (void *)vmm_phys_to_virt(vmm_active_pml4);
    vmm_serial_write("VMM: copying existing PML4 into new PML4\n");
    for (size_t i = 0; i < VMM_PAGE_SIZE/8; ++i) {
        ((uint64_t *)new_pml4)[i] = ((uint64_t *)old_pml4)[i];
    }

    vmm_serial_write("VMM: switching address space (cli)\n");
    __asm__ volatile ("cli");
    vm_switch_address_space(vmm_active_pml4);
    __asm__ volatile ("sti");
    vmm_serial_write("VMM: switched CR3 (sti)\n");

    vmm_initialized = true;
}

void vm_self_test(void) {
    if (!vmm_initialized) {
        kernel_panic("VMM self-test failed: VMM not initialized");
    }

    uint64_t physical_page = pmm_alloc_page();
    if (physical_page == 0U) {
        kernel_panic("VMM self-test failed: physical page allocation failed");
    }

    uint64_t virtual_address = 0x100000000ULL;
    if (!vm_map_page(virtual_address, physical_page, VMM_PAGE_PRESENT | VMM_PAGE_WRITABLE)) {
        kernel_panic("VMM self-test failed: map_page failed");
    }

    uint64_t translated = 0;
    if (!vm_get_physical(virtual_address, &translated) || translated != physical_page) {
        kernel_panic("VMM self-test failed: translation mismatch");
    }

    uint64_t *page = (uint64_t *)virtual_address;
    page[0] = 0xDEADBEEFCAFEBABEULL;
    if (page[0] != 0xDEADBEEFCAFEBABEULL) {
        kernel_panic("VMM self-test failed: mapped page not writable");
    }

    if (!vm_unmap_page(virtual_address)) {
        kernel_panic("VMM self-test failed: unmap_page failed");
    }

    if (vm_get_physical(virtual_address, &translated)) {
        kernel_panic("VMM self-test failed: unmapped page still resolves");
    }

    pmm_free_page(physical_page);
}
