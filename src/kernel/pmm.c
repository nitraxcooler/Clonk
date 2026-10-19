#include "kernel/pmm.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "boot/limine.h"
#include "kernel/panic.h"

#define PAGE_SIZE PMM_PAGE_SIZE
#define PAGE_MASK (~(uint64_t)(PAGE_SIZE - 1U))

extern uint8_t __kernel_start[];
extern uint8_t __kernel_end[];

extern volatile struct limine_memmap_request limine_memmap_request;
extern volatile struct limine_hhdm_request limine_hhdm_request;
extern volatile struct limine_executable_address_request limine_executable_address_request;

static uint8_t *pmm_bitmap = NULL;
static uint64_t pmm_bitmap_pages = 0;
static uint64_t pmm_bitmap_bytes = 0;
static uint64_t pmm_total_pages = 0;
static uint64_t pmm_free_pages = 0;
static uint64_t pmm_used_pages = 0;
static uint64_t pmm_total_memory = 0;
static uint64_t pmm_usable_memory = 0;
static uint64_t pmm_reserved_memory = 0;
static bool pmm_initialized = false;

static uint64_t align_up(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1U) & ~(alignment - 1U);
}

static uint64_t align_down(uint64_t value, uint64_t alignment) {
    return value & ~(alignment - 1U);
}

static uint64_t virt_to_phys(uint64_t virtual_address) {
    if (limine_hhdm_request.response != NULL) {
        return virtual_address - limine_hhdm_request.response->offset;
    }
    return virtual_address;
}

static uint64_t pmm_get_kernel_phys_base(void) {
    if (limine_executable_address_request.response != NULL) {
        return limine_executable_address_request.response->physical_base;
    }

    return virt_to_phys((uint64_t)__kernel_start);
}

static void pmm_set_bit(uint64_t page_index, bool used) {
    if (pmm_bitmap == NULL) {
        return;
    }
    if (page_index >= pmm_total_pages) {
        return;
    }

    uint64_t byte_index = page_index / 8U;
    uint8_t bit_index = (uint8_t)(page_index % 8U);
    uint8_t mask = (uint8_t)(1U << bit_index);

    if (used) {
        pmm_bitmap[byte_index] |= mask;
    } else {
        pmm_bitmap[byte_index] &= (uint8_t)(~mask);
    }
}

static bool pmm_get_bit(uint64_t page_index) {
    if (pmm_bitmap == NULL) {
        return true;
    }
    if (page_index >= pmm_total_pages) {
        return true;
    }

    uint64_t byte_index = page_index / 8U;
    uint8_t bit_index = (uint8_t)(page_index % 8U);
    return (pmm_bitmap[byte_index] & (1U << bit_index)) != 0U;
}

static void pmm_mark_range(uint64_t base, uint64_t length, bool used) {
    if (length == 0U) {
        return;
    }

    uint64_t start = align_up(base, PAGE_SIZE);
    uint64_t end = align_down(base + length, PAGE_SIZE);
    if (end <= start) {
        return;
    }

    for (uint64_t addr = start; addr < end; addr += PAGE_SIZE) {
        uint64_t page_index = addr / PAGE_SIZE;
        if (page_index >= pmm_total_pages) {
            continue;
        }
        pmm_set_bit(page_index, used);
    }
}

static void pmm_mark_region_free(uint64_t base, uint64_t length) {
    pmm_mark_range(base, length, false);
}

static void pmm_mark_region_used(uint64_t base, uint64_t length) {
    pmm_mark_range(base, length, true);
}

/* Find a usable memory region big enough to hold the page bitmap. */
static uint64_t pmm_find_bitmap_location(uint64_t kernel_start_phys, uint64_t kernel_end_phys) {
    if (limine_memmap_request.response == NULL) {
        kernel_panic("PMM memory map unavailable");
    }

    struct limine_memmap_entry **entries = limine_memmap_request.response->entries;
    uint64_t entry_count = limine_memmap_request.response->entry_count;

    for (uint64_t i = 0; i < entry_count; ++i) {
        struct limine_memmap_entry *entry = entries[i];
        if (entry == NULL) {
            continue;
        }
        if (entry->type != LIMINE_MEMMAP_USABLE) {
            continue;
        }

        uint64_t region_start = align_up(entry->base, PAGE_SIZE);
        uint64_t region_end = align_down(entry->base + entry->length, PAGE_SIZE);
        if (region_end <= region_start) {
            continue;
        }

        uint64_t required = pmm_bitmap_pages * PAGE_SIZE;
        uint64_t candidate = region_start;
        if (candidate < kernel_end_phys) {
            if (candidate < kernel_start_phys &&
                required <= kernel_start_phys - candidate) {
                if (required <= region_end - candidate) return candidate;
            }
            candidate = align_up(kernel_end_phys, PAGE_SIZE);
        }
        if (candidate <= region_end && required <= region_end - candidate) {
            return candidate;
        }
    }

    kernel_panic("PMM could not find usable bitmap region");
    return 0;
}

void pmm_init(void) {
    if (pmm_initialized) {
        return;
    }

    if (limine_memmap_request.response == NULL) {
        kernel_panic("PMM memory map response missing");
    }

    if (limine_hhdm_request.response == NULL) {
        kernel_panic("PMM HHDM response missing");
    }

    uint64_t kernel_start_phys = pmm_get_kernel_phys_base();
    uint64_t kernel_end_phys = kernel_start_phys + ((uint64_t)__kernel_end - (uint64_t)__kernel_start);
    uint64_t kernel_length = kernel_end_phys - kernel_start_phys;

    if (kernel_length == 0U) {
        kernel_length = PAGE_SIZE;
    }

    struct limine_memmap_entry **entries = limine_memmap_request.response->entries;
    uint64_t entry_count = limine_memmap_request.response->entry_count;

    uint64_t total_physical = 0;
    uint64_t usable_memory = 0;
    for (uint64_t i = 0; i < entry_count; ++i) {
        struct limine_memmap_entry *entry = entries[i];
        if (entry == NULL) {
            continue;
        }

        total_physical += entry->length;
        if (entry->type == LIMINE_MEMMAP_USABLE) {
            usable_memory += entry->length;
        }
    }

    pmm_total_memory = total_physical;
    pmm_usable_memory = usable_memory;
    pmm_reserved_memory = total_physical - usable_memory;

    pmm_total_pages = pmm_usable_memory / PAGE_SIZE;
    if (pmm_total_pages == 0U) {
        kernel_panic("PMM found no usable pages");
    }

    /* One bit per page: total_pages bits, rounded up to whole bytes. */
    pmm_bitmap_bytes = (pmm_total_pages + 7U) / 8U;
    pmm_bitmap_pages = align_up(pmm_bitmap_bytes, PAGE_SIZE) / PAGE_SIZE;

    uint64_t bitmap_base = pmm_find_bitmap_location(kernel_start_phys, kernel_end_phys);
    uint64_t bitmap_size_bytes = pmm_bitmap_pages * PAGE_SIZE;
    pmm_bitmap = (uint8_t *)(bitmap_base + limine_hhdm_request.response->offset);

    pmm_free_pages = 0;
    pmm_used_pages = 0;

    /* Start with every page marked used, then free the usable regions. */
    for (uint64_t page_index = 0; page_index < pmm_total_pages; ++page_index) {
        pmm_set_bit(page_index, true);
    }

    for (uint64_t i = 0; i < entry_count; ++i) {
        struct limine_memmap_entry *entry = entries[i];
        if (entry == NULL) {
            continue;
        }
        if (entry->type == LIMINE_MEMMAP_USABLE) {
            pmm_mark_region_free(entry->base, entry->length);
        }
    }

    /* Protect the first page, the kernel image, and the bitmap itself. */
    pmm_mark_region_used(0, PAGE_SIZE);
    pmm_mark_region_used(kernel_start_phys, kernel_length);
    pmm_mark_region_used(bitmap_base, bitmap_size_bytes);

    for (uint64_t index = 0; index < pmm_total_pages; ++index) {
        if (pmm_get_bit(index)) {
            ++pmm_used_pages;
        } else {
            ++pmm_free_pages;
        }
    }

    pmm_initialized = true;
}

uint64_t pmm_alloc_page(void) {
    if (!pmm_initialized) {
        kernel_panic("PMM used before initialization");
    }

    /* Skip page 0: the first page is always reserved. */
    for (uint64_t page_index = 1; page_index < pmm_total_pages; ++page_index) {
        if (!pmm_get_bit(page_index)) {
            pmm_set_bit(page_index, true);
            --pmm_free_pages;
            ++pmm_used_pages;
            return page_index * PAGE_SIZE;
        }
    }

    return 0;
}

void pmm_free_page(uint64_t physical_address) {
    if (physical_address == 0U) {
        return;
    }

    if (!pmm_initialized) {
        kernel_panic("PMM free before initialization");
    }

    if (physical_address < PAGE_SIZE) {
        return;
    }

    uint64_t page_index = physical_address / PAGE_SIZE;
    if (page_index >= pmm_total_pages) {
        return;
    }

    if (!pmm_get_bit(page_index)) {
        return;
    }

    pmm_set_bit(page_index, false);
    --pmm_used_pages;
    ++pmm_free_pages;
}

uint64_t pmm_get_total_memory(void) {
    return pmm_total_memory;
}

uint64_t pmm_get_usable_memory(void) {
    return pmm_usable_memory;
}

uint64_t pmm_get_reserved_memory(void) {
    return pmm_reserved_memory;
}

uint64_t pmm_get_total_pages(void) {
    return pmm_total_pages;
}

uint64_t pmm_get_free_pages(void) {
    return pmm_free_pages;
}

uint64_t pmm_get_used_pages(void) {
    return pmm_used_pages;
}

bool pmm_is_initialized(void) {
    return pmm_initialized;
}

void pmm_self_test(void) {
    if (!pmm_initialized) {
        kernel_panic("PMM self-test failed: PMM not initialized");
    }

    uint64_t first = pmm_alloc_page();
    if (first == 0U || (first % PAGE_SIZE) != 0U) {
        kernel_panic("PMM self-test failed: bad page allocation");
    }

    uint64_t second = pmm_alloc_page();
    if (second == 0U || second == first) {
        kernel_panic("PMM self-test failed: duplicate page allocation");
    }

    uint64_t free_before = pmm_get_free_pages();
    pmm_free_page(first);
    if (pmm_get_free_pages() != (free_before + 1U)) {
        kernel_panic("PMM self-test failed: free count mismatch");
    }

    uint64_t reallocated = pmm_alloc_page();
    if (reallocated != first) {
        kernel_panic("PMM self-test failed: reallocation mismatch");
    }

    pmm_free_page(first);
    pmm_free_page(second);
}
