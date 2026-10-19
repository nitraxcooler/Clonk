#include "kernel/usercopy.h"

#include "kernel/vmm.h"
#include "lib/mem.h"

bool user_copy_from(uint64_t address_space, void *kernel_destination,
                    uint64_t user_source, size_t length) {
    if (length == 0U) return true;
    if (kernel_destination == NULL ||
        !vm_user_range_valid(address_space, user_source, (uint64_t)length)) {
        return false;
    }
    memcpy(kernel_destination, (const void *)(uintptr_t)user_source, length);
    return true;
}

bool user_copy_to(uint64_t address_space, uint64_t user_destination,
                  const void *kernel_source, size_t length) {
    if (length == 0U) return true;
    if (kernel_source == NULL ||
        !vm_user_range_writable(address_space, user_destination,
                                (uint64_t)length)) {
        return false;
    }
    memcpy((void *)(uintptr_t)user_destination, kernel_source, length);
    return true;
}
