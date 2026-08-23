#ifndef VMM_H
#define VMM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VMM_PAGE_SIZE 4096UL
#define VMM_PAGE_PRESENT        0x1ULL
#define VMM_PAGE_WRITABLE       0x2ULL
#define VMM_PAGE_USER           0x4ULL
#define VMM_PAGE_WRITE_THROUGH  0x8ULL
#define VMM_PAGE_CACHE_DISABLE  0x10ULL
#define VMM_PAGE_ACCESSED       0x20ULL
#define VMM_PAGE_DIRTY          0x40ULL
#define VMM_PAGE_LARGE          0x80ULL
#define VMM_PAGE_GLOBAL         0x100ULL
#define VMM_PAGE_NX             0x8000000000000000ULL

void vm_init(void);
void vm_self_test(void);

uint64_t vm_create_address_space(void);
void vm_switch_address_space(uint64_t pml4_physical_address);
uint64_t vm_get_active_pml4(void);
bool vm_is_initialized(void);

bool vm_map_page(uint64_t virtual_address, uint64_t physical_address, uint64_t flags);
bool vm_unmap_page(uint64_t virtual_address);
bool vm_get_physical(uint64_t virtual_address, uint64_t *physical_address);

/* Convert a physical address to its higher-half identity virtual mapping. */
uint64_t vm_phys_to_virt(uint64_t physical_address);

uint64_t vm_get_mapped_pages(void);
uint64_t vm_get_page_table_count(void);

#endif
