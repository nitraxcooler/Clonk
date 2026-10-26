#include "kernel/process.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "abi/syscall.h"
#include "arch/x86_64/gdt.h"
#include "drivers/keyboard/keyboard.h"
#include "kernel/elf.h"
#include "kernel/heap.h"
#include "kernel/panic.h"
#include "kernel/usercopy.h"
#include "kernel/vmm.h"
#include "lib/mem.h"

#define PROCESS_KERNEL_STACK_SIZE 16384U
#define PROCESS_FRAME_WORDS 20U
#define PROCESS_FRAME_RAX 14U
#define PROCESS_FRAME_RIP 15U
#define PROCESS_FRAME_CS 16U
#define PROCESS_FRAME_RFLAGS 17U
#define PROCESS_FRAME_RSP 18U
#define PROCESS_FRAME_SS 19U

typedef enum {
    PROCESS_FREE,
    PROCESS_READY,
    PROCESS_RUNNING,
    PROCESS_BLOCKED,
    PROCESS_ZOMBIE
} process_state;

typedef struct {
    uint64_t pid;
    uint64_t parent_pid;
    uint64_t address_space;
    uint64_t kernel_stack;
    uint64_t kernel_stack_top;
    uint64_t *saved_frame;
    userfs_context *filesystem;
    process_state state;
    int64_t exit_status;
    uint64_t wait_pid;
    uint64_t wait_status_address;
    bool wait_claimed;
} process_task;

static process_task process_table[PROCESS_TABLE_LIMIT];
static uint64_t deferred_stacks[PROCESS_TABLE_LIMIT];
static size_t deferred_stack_count;
static uint64_t kernel_address_space;
static uint64_t next_process_id = 1U;
static int current_process = -1;
static int scheduler_cursor = -1;
static bool reschedule_pending;

static process_task *find_process(uint64_t pid) {
    if (pid == 0U) return NULL;
    for (size_t i = 0; i < PROCESS_TABLE_LIMIT; ++i) {
        if (process_table[i].state != PROCESS_FREE &&
            process_table[i].pid == pid) {
            return &process_table[i];
        }
    }
    return NULL;
}

static void defer_kernel_stack(process_task *task) {
    if (task->kernel_stack != 0U &&
        deferred_stack_count < PROCESS_TABLE_LIMIT) {
        deferred_stacks[deferred_stack_count++] = task->kernel_stack;
        task->kernel_stack = 0U;
    }
}

static int find_free_slot(void) {
    for (size_t i = 1U; i < PROCESS_TABLE_LIMIT; ++i) {
        if (process_table[i].state == PROCESS_FREE) return (int)i;
    }
    return -1;
}

static bool write_address_space(uint64_t address_space, uint64_t destination,
                                const void *source, size_t length) {
    const uint8_t *bytes = source;
    if (!vm_user_range_writable(address_space, destination, length)) return false;
    for (size_t i = 0; i < length; ++i) {
        uint64_t physical;
        if (!vm_get_physical_in(address_space, destination + i, &physical)) {
            return false;
        }
        *(volatile uint8_t *)(uintptr_t)vm_phys_to_virt(physical) = bytes[i];
    }
    return true;
}

void process_reap_deferred_stacks(void) {
    for (size_t i = 0; i < deferred_stack_count; ++i) {
        kfree((void *)(uintptr_t)deferred_stacks[i]);
        deferred_stacks[i] = 0U;
    }
    deferred_stack_count = 0U;
}

static void release_address_space(process_task *task) {
    if (task->address_space == 0U) return;
    if (task->address_space == vm_get_active_pml4()) {
        kernel_panic("PROCESS_ADDRESS_SPACE_ACTIVE_DURING_REAP");
    }
    if (!vm_destroy_address_space(task->address_space)) {
        kernel_panic("PROCESS_ADDRESS_SPACE_RECLAIM_FAILED");
    }
    task->address_space = 0U;
}

static void reap_task(process_task *task) {
    if (task->filesystem != NULL) {
        userfs_destroy_context(task->filesystem);
        task->filesystem = NULL;
    }
    release_address_space(task);
    if (task->kernel_stack != 0U) {
        kfree((void *)(uintptr_t)task->kernel_stack);
    }
    memset(task, 0, sizeof(*task));
}

static bool has_live_children(uint64_t parent_pid) {
    for (size_t i = 0; i < PROCESS_TABLE_LIMIT; ++i) {
        if (process_table[i].state != PROCESS_FREE &&
            process_table[i].parent_pid == parent_pid) {
            return true;
        }
    }
    return false;
}

static void orphan_children(uint64_t parent_pid) {
    for (size_t i = 0; i < PROCESS_TABLE_LIMIT; ++i) {
        if (process_table[i].state != PROCESS_FREE &&
            process_table[i].parent_pid == parent_pid) {
            process_table[i].parent_pid = 0U;
        }
    }
}

static void wake_waiting_parent(process_task *child) {
    process_task *parent = find_process(child->parent_pid);
    if (parent == NULL || parent->state != PROCESS_BLOCKED ||
        parent->wait_pid != child->pid) return;

    int64_t status = child->exit_status;
    bool copied = parent->wait_status_address == 0U ||
                  write_address_space(parent->address_space,
                                      parent->wait_status_address,
                                      &status, sizeof(status));
    parent->saved_frame[PROCESS_FRAME_RAX] = copied ? child->pid : (uint64_t)-1;
    parent->wait_pid = 0U;
    parent->wait_status_address = 0U;
    parent->state = PROCESS_READY;
    child->wait_claimed = true;
}

static void return_orphaned_tasks_to_kernel(void) {
    vm_switch_address_space(kernel_address_space);
    arch_set_kernel_stack(arch_kernel_stack_top());
    for (size_t i = 0; i < PROCESS_TABLE_LIMIT; ++i) {
        process_task *task = &process_table[i];
        if (task->state != PROCESS_FREE) {
            if (task->filesystem != NULL) {
                userfs_destroy_context(task->filesystem);
            }
            if (task->address_space != 0U &&
                !vm_destroy_address_space(task->address_space)) {
                kernel_panic("PROCESS_ADDRESS_SPACE_RECLAIM_FAILED");
            }
            if (task->kernel_stack != 0U) {
                defer_kernel_stack(task);
            }
            memset(task, 0, sizeof(*task));
        }
    }
    current_process = -1;
    scheduler_cursor = -1;
    reschedule_pending = false;
    keyboard_set_user_mode(false);
    process_return_to_kernel();
}

static uint64_t *schedule_next(void) {
    int previous = current_process;
    for (;;) {
        for (size_t offset = 1U; offset <= PROCESS_TABLE_LIMIT; ++offset) {
            int index = (scheduler_cursor + (int)offset) %
                        (int)PROCESS_TABLE_LIMIT;
            process_task *candidate = &process_table[index];
            if (candidate->state != PROCESS_READY) continue;

            current_process = index;
            scheduler_cursor = index;
            candidate->state = PROCESS_RUNNING;
            reschedule_pending = false;
            vm_switch_address_space(candidate->address_space);
            arch_set_kernel_stack(candidate->kernel_stack_top != 0U
                                      ? candidate->kernel_stack_top
                                      : arch_kernel_stack_top());
            if (previous >= 0 && previous != index &&
                process_table[previous].state == PROCESS_ZOMBIE) {
                process_task *old = &process_table[previous];
                bool reap = old->parent_pid == 0U || old->wait_claimed;
                release_address_space(old);
                if (reap) {
                    defer_kernel_stack(old);
                    memset(old, 0, sizeof(*old));
                }
            }
            for (size_t i = 0; i < PROCESS_TABLE_LIMIT; ++i) {
                process_task *orphan = &process_table[i];
                if ((int)i != index && orphan->state == PROCESS_ZOMBIE &&
                    orphan->parent_pid == 0U) {
                    release_address_space(orphan);
                    defer_kernel_stack(orphan);
                    memset(orphan, 0, sizeof(*orphan));
                }
            }
            return candidate->saved_frame;
        }

        bool any_blocked = false;
        bool orphaned_zombie = false;
        for (size_t i = 0; i < PROCESS_TABLE_LIMIT; ++i) {
            if (process_table[i].state == PROCESS_BLOCKED) any_blocked = true;
            if (process_table[i].state == PROCESS_ZOMBIE &&
                process_table[i].parent_pid == 0U) {
                orphaned_zombie = true;
            }
        }
        if (orphaned_zombie && !any_blocked) {
            return_orphaned_tasks_to_kernel();
        }
        __asm__ volatile ("sti; hlt; cli" : : : "memory");
    }
}

bool process_manager_start_init(uint64_t address_space) {
    if (address_space == 0U || current_process >= 0) return false;
    memset(process_table, 0, sizeof(process_table));
    kernel_address_space = vm_get_active_pml4();
    if (kernel_address_space == 0U) return false;
    userfs_context *filesystem = userfs_create_context();
    if (filesystem == NULL) return false;

    process_table[0].pid = next_process_id++;
    process_table[0].address_space = address_space;
    process_table[0].kernel_stack_top = arch_kernel_stack_top();
    process_table[0].filesystem = filesystem;
    process_table[0].state = PROCESS_RUNNING;
    current_process = 0;
    scheduler_cursor = 0;
    reschedule_pending = false;
    arch_set_kernel_stack(arch_kernel_stack_top());
    return true;
}

uint64_t process_current_address_space(void) {
    if (current_process < 0) return 0U;
    return process_table[current_process].address_space;
}

uint64_t process_current_pid(void) {
    if (current_process < 0) return 0U;
    return process_table[current_process].pid;
}

userfs_context *process_current_userfs(void) {
    if (current_process < 0) return NULL;
    return process_table[current_process].filesystem;
}

int64_t process_spawn_image(const void *image, size_t image_size) {
    if (current_process < 0 || image == NULL || image_size == 0U) {
        return CLONK_PROCESS_ERR_INVALID;
    }
    int slot = find_free_slot();
    if (slot < 0) return CLONK_PROCESS_ERR_LIMIT;

    process_task task;
    memset(&task, 0, sizeof(task));
    task.kernel_stack = (uint64_t)(uintptr_t)kmalloc(PROCESS_KERNEL_STACK_SIZE);
    task.filesystem = userfs_create_context();
    if (task.kernel_stack == 0U || task.filesystem == NULL) {
        if (task.filesystem != NULL) userfs_destroy_context(task.filesystem);
        if (task.kernel_stack != 0U) kfree((void *)(uintptr_t)task.kernel_stack);
        return CLONK_PROCESS_ERR_NO_MEMORY;
    }

    uint64_t entry_point = 0U;
    uint64_t stack_pointer = 0U;
    if (!elf_load_user(image, image_size, &task.address_space,
                       &entry_point, &stack_pointer)) {
        userfs_destroy_context(task.filesystem);
        kfree((void *)(uintptr_t)task.kernel_stack);
        return CLONK_PROCESS_ERR_INVALID;
    }

    task.kernel_stack_top =
        ((task.kernel_stack + PROCESS_KERNEL_STACK_SIZE) & ~0xFULL);
    task.saved_frame = (uint64_t *)(uintptr_t)(
        task.kernel_stack_top - PROCESS_FRAME_WORDS * sizeof(uint64_t));
    memset(task.saved_frame, 0, PROCESS_FRAME_WORDS * sizeof(uint64_t));
    task.saved_frame[PROCESS_FRAME_RIP] = entry_point;
    task.saved_frame[PROCESS_FRAME_CS] = 0x23U;
    task.saved_frame[PROCESS_FRAME_RFLAGS] = 0x202U;
    task.saved_frame[PROCESS_FRAME_RSP] = stack_pointer;
    task.saved_frame[PROCESS_FRAME_SS] = 0x1BU;
    if (next_process_id == UINT64_MAX) {
        userfs_destroy_context(task.filesystem);
        (void)vm_destroy_address_space(task.address_space);
        kfree((void *)(uintptr_t)task.kernel_stack);
        return CLONK_PROCESS_ERR_LIMIT;
    }
    task.pid = next_process_id++;
    task.parent_pid = process_table[current_process].pid;
    task.state = PROCESS_READY;
    process_table[slot] = task;
    return (int64_t)task.pid;
}

int64_t process_wait(uint64_t pid, uint64_t status_address) {
    if (current_process < 0 || pid == 0U || pid == process_current_pid()) {
        return CLONK_PROCESS_ERR_INVALID;
    }
    process_task *parent = &process_table[current_process];
    process_task *child = find_process(pid);
    if (child == NULL || child->parent_pid != parent->pid) {
        return CLONK_PROCESS_ERR_INVALID;
    }
    if (status_address != 0U &&
        !vm_user_range_writable(parent->address_space, status_address,
                                sizeof(int64_t))) return CLONK_PROCESS_ERR_INVALID;

    if (child->state == PROCESS_ZOMBIE) {
        int64_t status = child->exit_status;
        if (status_address != 0U &&
            !user_copy_to(parent->address_space, status_address,
                          &status, sizeof(status))) return CLONK_PROCESS_ERR_INVALID;
        reap_task(child);
        return (int64_t)pid;
    }
    parent->wait_pid = pid;
    parent->wait_status_address = status_address;
    parent->state = PROCESS_BLOCKED;
    reschedule_pending = true;
    return 0;
}

void process_request_yield(void) {
    reschedule_pending = true;
}

uint64_t *process_syscall_return(uint64_t *frame) {
    process_reap_deferred_stacks();
    if (current_process < 0) return frame;
    process_task *current = &process_table[current_process];
    if (current->state != PROCESS_ZOMBIE && current->state != PROCESS_FREE) {
        current->saved_frame = frame;
    }
    if (current->state == PROCESS_RUNNING && !reschedule_pending) return frame;
    if (current->state == PROCESS_RUNNING) current->state = PROCESS_READY;
    return schedule_next();
}

uint64_t *process_timer_interrupt(uint64_t *frame) {
    reschedule_pending = true;
    if (current_process < 0 || (frame[PROCESS_FRAME_CS] & 3U) != 3U) {
        return frame;
    }
    process_task *current = &process_table[current_process];
    if (current->state == PROCESS_RUNNING) current->saved_frame = frame;
    return process_syscall_return(frame);
}

uint64_t *process_terminate_current(int64_t status) {
    if (current_process < 0) return NULL;
    process_task *current = &process_table[current_process];
    current->exit_status = status;
    current->state = PROCESS_ZOMBIE;
    if (current->filesystem != NULL) {
        userfs_destroy_context(current->filesystem);
        current->filesystem = NULL;
    }
    orphan_children(current->pid);
    if (current->parent_pid != 0U) {
        wake_waiting_parent(current);
    }
    return schedule_next();
}

bool process_exit_current(int64_t status) {
    if (current_process < 0) return false;
    process_task *current = &process_table[current_process];
    if (has_live_children(current->pid)) return false;
    current->exit_status = status;
    current->state = PROCESS_ZOMBIE;
    reschedule_pending = true;
    if (current->filesystem != NULL) {
        userfs_destroy_context(current->filesystem);
        current->filesystem = NULL;
    }
    if (current->parent_pid != 0U) {
        wake_waiting_parent(current);
    }
    return true;
}

bool process_prepare_init_exit(void) {
    if (current_process != 0 || has_live_children(process_current_pid())) {
        return false;
    }
    process_task *init = &process_table[0];
    if (init->filesystem != NULL) {
        userfs_destroy_context(init->filesystem);
        init->filesystem = NULL;
    }
    vm_switch_address_space(kernel_address_space);
    arch_set_kernel_stack(arch_kernel_stack_top());
    release_address_space(init);
    memset(init, 0, sizeof(*init));
    current_process = -1;
    scheduler_cursor = -1;
    reschedule_pending = false;
    return true;
}
