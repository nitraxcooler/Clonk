#include "kernel/elf.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kernel/pmm.h"
#include "kernel/vmm.h"
#include "arch/x86_64/gdt.h"
#include "kernel/panic.h"
#include "lib/mem.h"

#define ELF_PT_NULL 0U
#define ELF_PT_LOAD 1U
#define ELF_PT_NOTE 4U
#define ELF_PT_PHDR 6U
#define ELF_PT_GNU_EH_FRAME 0x6474E550U
#define ELF_PT_GNU_STACK 0x6474E551U
#define ELF_PF_R 4U
#define ELF_PF_X 1U
#define ELF_PF_W 2U
#define USER_MIN_ADDRESS 0x0000000000400000ULL
#define USER_MAX_ADDRESS 0x0000800000000000ULL
#define USER_STACK_TOP 0x00007FFFFFFFE000ULL
#define USER_STACK_PAGES 4U
#define USER_STACK_BOTTOM (USER_STACK_TOP - USER_STACK_PAGES * VMM_PAGE_SIZE)
#define USER_STACK_GUARD_BOTTOM (USER_STACK_BOTTOM - VMM_PAGE_SIZE)
#define ELF_MAX_PROGRAM_HEADERS 128U
#define ELF_MAX_IMAGE_PAGES 16384U
#define ELF_TEST_IMAGE_SIZE 0x2001U

struct elf64_header {
    uint8_t ident[16];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint64_t entry;
    uint64_t program_offset;
    uint64_t section_offset;
    uint32_t flags;
    uint16_t header_size;
    uint16_t program_entry_size;
    uint16_t program_count;
    uint16_t section_entry_size;
    uint16_t section_count;
    uint16_t section_names_index;
} __attribute__((packed));

struct elf64_program_header {
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t virtual_address;
    uint64_t physical_address;
    uint64_t file_size;
    uint64_t memory_size;
    uint64_t alignment;
} __attribute__((packed));

struct elf_segment_layout {
    uint64_t end;
    uint64_t page_start;
    uint64_t page_end;
    bool load;
};

static bool range_inside(uint64_t offset, uint64_t length, size_t total) {
    return offset <= total && length <= (uint64_t)total - offset;
}

static bool add_overflow(uint64_t a, uint64_t b, uint64_t *sum) {
    *sum = a + b;
    return *sum < a;
}

static bool validate_segment(const struct elf64_program_header *segment, size_t image_size,
                             struct elf_segment_layout *layout) {
    uint64_t segment_end;
    if (segment->file_size > segment->memory_size ||
        !range_inside(segment->offset, segment->file_size, image_size) ||
        (segment->flags & ~7U) != 0U ||
        ((segment->flags & (ELF_PF_W | ELF_PF_X)) == (ELF_PF_W | ELF_PF_X)) ||
        (segment->alignment > 1U &&
         ((segment->alignment & (segment->alignment - 1U)) != 0U ||
          (segment->virtual_address & (segment->alignment - 1U)) !=
          (segment->offset & (segment->alignment - 1U)))) ||
        add_overflow(segment->virtual_address, segment->memory_size, &segment_end)) return false;

    layout->end = segment_end;
    layout->load = segment->memory_size != 0U;
    if (!layout->load) return true;
    if (segment->virtual_address < USER_MIN_ADDRESS ||
        segment_end > USER_MAX_ADDRESS || segment_end > USER_STACK_GUARD_BOTTOM) return false;
    layout->page_start = segment->virtual_address & ~0xFFFULL;
    if (add_overflow(segment_end, 0xFFFULL, &layout->page_end)) return false;
    layout->page_end &= ~0xFFFULL;
    return layout->page_end > layout->page_start;
}

static bool map_segment(uint64_t address_space, const uint8_t *image,
                        const struct elf64_program_header *segment,
                        const struct elf_segment_layout *layout) {
    uint64_t page_flags = VMM_PAGE_USER;
    if ((segment->flags & ELF_PF_W) != 0U) page_flags |= VMM_PAGE_WRITABLE;
    if ((segment->flags & ELF_PF_X) == 0U && arch_nx_enabled()) page_flags |= VMM_PAGE_NX;

    for (uint64_t address = layout->page_start; address < layout->page_end;
         address += VMM_PAGE_SIZE) {
        uint64_t physical = pmm_alloc_page();
        if (physical == 0U) return false;
        memset((void *)(uintptr_t)vm_phys_to_virt(physical), 0, VMM_PAGE_SIZE);
        if (!vm_map_page_in(address_space, address, physical, page_flags)) {
            pmm_free_page(physical);
            return false;
        }
    }

    for (uint64_t i = 0; i < segment->file_size; ++i) {
        uint64_t physical;
        if (!vm_get_physical_in(address_space, segment->virtual_address + i, &physical)) return false;
        *(uint8_t *)(uintptr_t)vm_phys_to_virt(physical) = image[segment->offset + i];
    }
    return true;
}

bool elf_load_user(const void *image_pointer, size_t image_size, uint64_t *address_space,
                   uint64_t *entry_point, uint64_t *stack_pointer) {
    if (image_pointer == NULL || address_space == NULL || entry_point == NULL ||
        stack_pointer == NULL || image_size < sizeof(struct elf64_header)) return false;

    const uint8_t *image = (const uint8_t *)image_pointer;
    const struct elf64_header *header = (const struct elf64_header *)image;
    if (header->ident[0] != 0x7FU || header->ident[1] != 'E' ||
        header->ident[2] != 'L' || header->ident[3] != 'F' ||
        header->ident[4] != 2U || header->ident[5] != 1U ||
        header->ident[6] != 1U || header->type != 2U ||
        header->machine != 62U || header->version != 1U ||
        header->header_size != sizeof(*header) ||
        header->program_entry_size != sizeof(struct elf64_program_header) ||
        header->program_count == 0U || header->program_count > ELF_MAX_PROGRAM_HEADERS ||
        !range_inside(header->program_offset,
            (uint64_t)header->program_count * header->program_entry_size, image_size)) return false;

    bool entry_executable = false;
    bool loaded_segment = false;
    uint64_t image_pages = 0;
    uint64_t load_segment_count = 0;
    struct elf_segment_layout layouts[ELF_MAX_PROGRAM_HEADERS];
    const struct elf64_program_header *programs =
        (const struct elf64_program_header *)(image + header->program_offset);
    for (uint16_t i = 0; i < header->program_count; ++i) {
        const struct elf64_program_header *program = &programs[i];
        if (program->type != ELF_PT_LOAD) {
            if (program->type == ELF_PT_NULL || program->type == ELF_PT_NOTE ||
                program->type == ELF_PT_PHDR || program->type == ELF_PT_GNU_EH_FRAME) {
                continue;
            }
            if (program->type == ELF_PT_GNU_STACK &&
                (program->flags & ELF_PF_X) == 0U) continue;
            return false;
        }
        if (!validate_segment(program, image_size, &layouts[i])) return false;
        if (!layouts[i].load) continue;
        uint64_t segment_pages = (layouts[i].page_end - layouts[i].page_start) / VMM_PAGE_SIZE;
        if (segment_pages > ELF_MAX_IMAGE_PAGES - image_pages) return false;
        image_pages += segment_pages;
        ++load_segment_count;
        for (uint16_t j = 0; j < i; ++j) {
            if (programs[j].type == ELF_PT_LOAD && layouts[j].load &&
                layouts[i].page_start < layouts[j].page_end &&
                layouts[j].page_start < layouts[i].page_end) return false;
        }
        loaded_segment = true;
        if ((program->flags & ELF_PF_X) != 0U &&
            header->entry >= program->virtual_address && header->entry < layouts[i].end) {
            entry_executable = true;
        }
    }

    if (!loaded_segment || !entry_executable) {
        return false;
    }
    uint64_t required_pages = image_pages + USER_STACK_PAGES +
                              (load_segment_count + 1U) * 3U + 1U;
    if (required_pages > pmm_get_free_pages()) return false;

    uint64_t space = vm_create_address_space();
    if (space == 0U) return false;
    for (uint16_t i = 0; i < header->program_count; ++i) {
        if (programs[i].type != ELF_PT_LOAD || !layouts[i].load) continue;
        if (!map_segment(space, image, &programs[i], &layouts[i])) {
            (void)vm_destroy_address_space(space);
            return false;
        }
    }

    for (uint64_t i = 1; i <= USER_STACK_PAGES; ++i) {
        uint64_t address = USER_STACK_TOP - i * VMM_PAGE_SIZE;
        uint64_t physical = pmm_alloc_page();
        if (physical == 0U) {
            (void)vm_destroy_address_space(space);
            return false;
        }
        memset((void *)(uintptr_t)vm_phys_to_virt(physical), 0, VMM_PAGE_SIZE);
        if (!vm_map_page_in(space, address, physical,
                            VMM_PAGE_USER | VMM_PAGE_WRITABLE |
                            (arch_nx_enabled() ? VMM_PAGE_NX : 0U))) {
            pmm_free_page(physical);
            (void)vm_destroy_address_space(space);
            return false;
        }
    }

    *address_space = space;
    *entry_point = header->entry;
    *stack_pointer = USER_STACK_TOP;
    return true;
}

static uint8_t elf_test_image[ELF_TEST_IMAGE_SIZE];

static void elf_test_build_image(void) {
    memset(elf_test_image, 0, sizeof(elf_test_image));
    struct elf64_header *header = (struct elf64_header *)elf_test_image;
    header->ident[0] = 0x7FU;
    header->ident[1] = 'E';
    header->ident[2] = 'L';
    header->ident[3] = 'F';
    header->ident[4] = 2U;
    header->ident[5] = 1U;
    header->ident[6] = 1U;
    header->type = 2U;
    header->machine = 62U;
    header->version = 1U;
    header->entry = USER_MIN_ADDRESS;
    header->program_offset = sizeof(*header);
    header->header_size = sizeof(*header);
    header->program_entry_size = sizeof(struct elf64_program_header);
    header->program_count = 2U;

    struct elf64_program_header *programs =
        (struct elf64_program_header *)(elf_test_image + header->program_offset);
    programs[0].type = ELF_PT_LOAD;
    programs[0].flags = ELF_PF_R | ELF_PF_X;
    programs[0].offset = 0x1000U;
    programs[0].virtual_address = USER_MIN_ADDRESS;
    programs[0].file_size = 1U;
    programs[0].memory_size = VMM_PAGE_SIZE;
    programs[0].alignment = VMM_PAGE_SIZE;

    programs[1].type = ELF_PT_LOAD;
    programs[1].flags = ELF_PF_R | ELF_PF_W;
    programs[1].offset = 0x2000U;
    programs[1].virtual_address = USER_MIN_ADDRESS + 2U * VMM_PAGE_SIZE;
    programs[1].file_size = 1U;
    programs[1].memory_size = 2U * VMM_PAGE_SIZE;
    programs[1].alignment = VMM_PAGE_SIZE;
}

static void elf_test_expect_rejected(const void *image, size_t image_size) {
    uint64_t free_before = pmm_get_free_pages();
    uint64_t mapped_before = vm_get_mapped_pages();
    uint64_t tables_before = vm_get_page_table_count();
    uint64_t address_space = 0;
    uint64_t entry_point = 0;
    uint64_t stack_pointer = 0;
    if (elf_load_user(image, image_size, &address_space, &entry_point, &stack_pointer) ||
        pmm_get_free_pages() != free_before ||
        vm_get_mapped_pages() != mapped_before ||
        vm_get_page_table_count() != tables_before) {
        kernel_panic("ELF_SELF_TEST_REJECTION_FAILED");
    }
}

void elf_self_test(void) {
    if (!pmm_is_initialized() || !vm_is_initialized()) {
        kernel_panic("ELF_SELF_TEST_NOT_INITIALIZED");
    }
    elf_test_build_image();

    uint64_t free_before = pmm_get_free_pages();
    uint64_t mapped_before = vm_get_mapped_pages();
    uint64_t tables_before = vm_get_page_table_count();
    uint64_t address_space = 0;
    uint64_t entry_point = 0;
    uint64_t stack_pointer = 0;
    if (!elf_load_user(elf_test_image, sizeof(elf_test_image), &address_space,
                       &entry_point, &stack_pointer) ||
        entry_point != USER_MIN_ADDRESS || stack_pointer != USER_STACK_TOP ||
        !vm_user_range_valid(address_space, USER_MIN_ADDRESS, 1U) ||
        !vm_user_range_valid(address_space, USER_MIN_ADDRESS + 2U * VMM_PAGE_SIZE, 1U) ||
        !vm_user_range_valid(address_space, USER_STACK_BOTTOM, 1U) ||
        vm_user_range_valid(address_space, USER_STACK_GUARD_BOTTOM, 1U)) {
        if (address_space != 0U) (void)vm_destroy_address_space(address_space);
        kernel_panic("ELF_SELF_TEST_LOAD_FAILED");
    }

    uint64_t code_flags = 0;
    uint64_t data_flags = 0;
    uint64_t stack_flags = 0;
    if (!vm_get_page_flags_in(address_space, USER_MIN_ADDRESS, &code_flags) ||
        !vm_get_page_flags_in(address_space, USER_MIN_ADDRESS + 2U * VMM_PAGE_SIZE,
                              &data_flags) ||
        !vm_get_page_flags_in(address_space, USER_STACK_BOTTOM, &stack_flags)) {
        (void)vm_destroy_address_space(address_space);
        kernel_panic("ELF_SELF_TEST_PERMISSIONS_FAILED");
    }
    uint64_t expected_nx = arch_nx_enabled() ? VMM_PAGE_NX : 0U;
    if ((code_flags & (VMM_PAGE_USER | VMM_PAGE_WRITABLE | VMM_PAGE_NX)) !=
        VMM_PAGE_USER) {
        (void)vm_destroy_address_space(address_space);
        kernel_panic("ELF_SELF_TEST_CODE_FLAGS_FAILED");
    }
    if ((data_flags & (VMM_PAGE_USER | VMM_PAGE_WRITABLE | VMM_PAGE_NX)) !=
        (VMM_PAGE_USER | VMM_PAGE_WRITABLE | expected_nx)) {
        (void)vm_destroy_address_space(address_space);
        kernel_panic("ELF_SELF_TEST_DATA_FLAGS_FAILED");
    }
    if ((stack_flags & (VMM_PAGE_USER | VMM_PAGE_WRITABLE | VMM_PAGE_NX)) !=
        (VMM_PAGE_USER | VMM_PAGE_WRITABLE | expected_nx)) {
        (void)vm_destroy_address_space(address_space);
        kernel_panic("ELF_SELF_TEST_STACK_FLAGS_FAILED");
    }
    if (!vm_destroy_address_space(address_space) ||
        pmm_get_free_pages() != free_before ||
        vm_get_mapped_pages() != mapped_before ||
        vm_get_page_table_count() != tables_before) {
        kernel_panic("ELF_SELF_TEST_RECLAIM_FAILED");
    }

    struct elf64_header *header = (struct elf64_header *)elf_test_image;
    struct elf64_program_header *programs =
        (struct elf64_program_header *)(elf_test_image + header->program_offset);
    elf_test_image[0] = 0;
    elf_test_expect_rejected(elf_test_image, sizeof(elf_test_image));
    elf_test_build_image();
    elf_test_expect_rejected(elf_test_image, sizeof(struct elf64_header) - 1U);
    elf_test_build_image();
    header = (struct elf64_header *)elf_test_image;
    header->ident[4] = 1U;
    elf_test_expect_rejected(elf_test_image, sizeof(elf_test_image));
    elf_test_build_image();
    header = (struct elf64_header *)elf_test_image;
    header->ident[5] = 2U;
    elf_test_expect_rejected(elf_test_image, sizeof(elf_test_image));
    elf_test_build_image();
    header = (struct elf64_header *)elf_test_image;
    header->header_size = 0U;
    elf_test_expect_rejected(elf_test_image, sizeof(elf_test_image));
    elf_test_build_image();
    header = (struct elf64_header *)elf_test_image;
    header->program_entry_size = 0U;
    elf_test_expect_rejected(elf_test_image, sizeof(elf_test_image));
    elf_test_build_image();
    header = (struct elf64_header *)elf_test_image;
    header->program_count = ELF_MAX_PROGRAM_HEADERS + 1U;
    elf_test_expect_rejected(elf_test_image, sizeof(elf_test_image));
    elf_test_build_image();
    header = (struct elf64_header *)elf_test_image;
    header->program_offset = UINT64_MAX - 8U;
    elf_test_expect_rejected(elf_test_image, sizeof(elf_test_image));
    elf_test_build_image();
    header = (struct elf64_header *)elf_test_image;
    programs = (struct elf64_program_header *)(elf_test_image + header->program_offset);
    programs[0].type = 3U;
    elf_test_expect_rejected(elf_test_image, sizeof(elf_test_image));
    elf_test_build_image();
    header = (struct elf64_header *)elf_test_image;
    programs = (struct elf64_program_header *)(elf_test_image + header->program_offset);
    programs[0].type = 2U;
    elf_test_expect_rejected(elf_test_image, sizeof(elf_test_image));
    elf_test_build_image();
    header = (struct elf64_header *)elf_test_image;
    programs = (struct elf64_program_header *)(elf_test_image + header->program_offset);
    programs[0].type = 7U;
    elf_test_expect_rejected(elf_test_image, sizeof(elf_test_image));
    elf_test_build_image();
    header = (struct elf64_header *)elf_test_image;
    programs = (struct elf64_program_header *)(elf_test_image + header->program_offset);
    programs[0].type = 0x60000000U;
    elf_test_expect_rejected(elf_test_image, sizeof(elf_test_image));
    elf_test_build_image();
    header = (struct elf64_header *)elf_test_image;
    programs = (struct elf64_program_header *)(elf_test_image + header->program_offset);
    programs[0].flags |= ELF_PF_W;
    elf_test_expect_rejected(elf_test_image, sizeof(elf_test_image));
    elf_test_build_image();
    header = (struct elf64_header *)elf_test_image;
    programs = (struct elf64_program_header *)(elf_test_image + header->program_offset);
    programs[1].virtual_address = programs[0].virtual_address;
    elf_test_expect_rejected(elf_test_image, sizeof(elf_test_image));
    elf_test_build_image();
    header = (struct elf64_header *)elf_test_image;
    programs = (struct elf64_program_header *)(elf_test_image + header->program_offset);
    programs[1].file_size = programs[1].memory_size + 1U;
    elf_test_expect_rejected(elf_test_image, sizeof(elf_test_image));
    elf_test_build_image();
    header = (struct elf64_header *)elf_test_image;
    programs = (struct elf64_program_header *)(elf_test_image + header->program_offset);
    programs[1].offset = ELF_TEST_IMAGE_SIZE;
    elf_test_expect_rejected(elf_test_image, sizeof(elf_test_image));
    elf_test_build_image();
    header = (struct elf64_header *)elf_test_image;
    programs = (struct elf64_program_header *)(elf_test_image + header->program_offset);
    programs[1].alignment = 3U;
    elf_test_expect_rejected(elf_test_image, sizeof(elf_test_image));
    elf_test_build_image();
    header = (struct elf64_header *)elf_test_image;
    programs = (struct elf64_program_header *)(elf_test_image + header->program_offset);
    programs[1].virtual_address = UINT64_MAX - 0xFFFU;
    programs[1].memory_size = 0x2000U;
    elf_test_expect_rejected(elf_test_image, sizeof(elf_test_image));
    elf_test_build_image();
    header = (struct elf64_header *)elf_test_image;
    programs = (struct elf64_program_header *)(elf_test_image + header->program_offset);
    programs[1].virtual_address = USER_MAX_ADDRESS;
    elf_test_expect_rejected(elf_test_image, sizeof(elf_test_image));
    elf_test_build_image();
    header = (struct elf64_header *)elf_test_image;
    programs = (struct elf64_program_header *)(elf_test_image + header->program_offset);
    programs[1].virtual_address = USER_STACK_GUARD_BOTTOM;
    programs[1].memory_size = VMM_PAGE_SIZE;
    elf_test_expect_rejected(elf_test_image, sizeof(elf_test_image));
    elf_test_build_image();
    header = (struct elf64_header *)elf_test_image;
    header->entry = USER_MIN_ADDRESS + 1U;
    programs = (struct elf64_program_header *)(elf_test_image + header->program_offset);
    programs[0].flags = ELF_PF_R;
    elf_test_expect_rejected(elf_test_image, sizeof(elf_test_image));
    elf_test_build_image();
    header = (struct elf64_header *)elf_test_image;
    header->machine = 3U;
    elf_test_expect_rejected(elf_test_image, sizeof(elf_test_image));

    memset(elf_test_image, 0, sizeof(elf_test_image));
    (void)programs;
}
