#include "kernel/userfs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "abi/syscall.h"
#include "fs/vfs.h"
#include "kernel/heap.h"
#include "kernel/usercopy.h"
#include "kernel/vmm.h"
#include "lib/mem.h"
#include "lib/string.h"

typedef struct {
    bool in_use;
    uint32_t flags;
    uint64_t offset;
    uint32_t directory_index;
    vfs_file file;
    char path[MYOS_FS_MAX_PATH];
} userfs_handle;

struct userfs_context {
    userfs_handle handles[MYOS_FS_HANDLE_LIMIT];
};

static bool short_name_valid(const char *name, size_t length) {
    size_t base_length = 0;
    size_t extension_length = 0;
    bool in_extension = false;
    if (length == 0U) return false;

    for (size_t i = 0; i < length; ++i) {
        char c = name[i];
        if (c == '.') {
            if (in_extension || base_length == 0U) return false;
            in_extension = true;
            continue;
        }
        bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                       (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '~';
        if (!allowed) return false;
        if (in_extension) {
            if (++extension_length > 3U) return false;
        } else if (++base_length > 8U) {
            return false;
        }
    }
    return base_length != 0U && (!in_extension || extension_length != 0U);
}

static bool normalize_user_path(char path[MYOS_FS_MAX_PATH]) {
    size_t length = 0;
    while (length < MYOS_FS_MAX_PATH && path[length] != '\0') ++length;
    if (length == 0U || length == MYOS_FS_MAX_PATH) {
        return false;
    }
    if (path[0] != '/') {
        if (length + 1U >= MYOS_FS_MAX_PATH) return false;
        for (size_t i = length + 1U; i > 0U; --i) path[i] = path[i - 1U];
        path[0] = '/';
        ++length;
    }

    size_t read = 1U;
    size_t write = 1U;
    while (read < length) {
        while (read < length && path[read] == '/') ++read;
        if (read == length) break;

        size_t start = read;
        while (read < length && path[read] != '/') ++read;
        size_t component_length = read - start;
        if (component_length == 1U && path[start] == '.') return false;
        if (component_length == 2U && path[start] == '.' && path[start + 1U] == '.') {
            return false;
        }
        if (component_length > 12U ||
            !short_name_valid(path + start, component_length)) {
            return false;
        }

        if (write > 1U) path[write++] = '/';
        for (size_t i = 0; i < component_length; ++i) {
            path[write++] = path[start + i];
        }
    }
    path[write] = '\0';
    return true;
}

static int copy_user_path(uint64_t address_space, uint64_t user_path,
                          char path[MYOS_FS_MAX_PATH]) {
    const uint64_t user_limit = 0x0000800000000000ULL;
    if (user_path >= user_limit) return MYOS_FS_ERR_PATH;

    for (size_t i = 0; i < MYOS_FS_MAX_PATH; ++i) {
        if ((uint64_t)i > user_limit - user_path ||
            !user_copy_from(address_space, &path[i],
                            user_path + (uint64_t)i, 1U)) {
            return MYOS_FS_ERR_PATH;
        }
        char value = path[i];
        if (value == '\0') {
            return normalize_user_path(path) ? 0 : MYOS_FS_ERR_PATH;
        }
    }
    return MYOS_FS_ERR_PATH;
}

static int64_t map_vfs_error(int error) {
    switch (error) {
        case VFS_ERR_INVALID: return MYOS_FS_ERR_INVALID;
        case VFS_ERR_NOT_FOUND: return MYOS_FS_ERR_NOT_FOUND;
        case VFS_ERR_NOT_DIRECTORY: return MYOS_FS_ERR_NOT_DIRECTORY;
        case VFS_ERR_IS_DIRECTORY: return MYOS_FS_ERR_IS_DIRECTORY;
        case VFS_ERR_EXISTS: return MYOS_FS_ERR_EXISTS;
        case VFS_ERR_READ_ONLY: return MYOS_FS_ERR_READ_ONLY;
        case VFS_ERR_NO_MEMORY: return MYOS_FS_ERR_NO_MEMORY;
        case VFS_ERR_NO_SPACE: return MYOS_FS_ERR_NO_SPACE;
        case VFS_ERR_LIMIT: return MYOS_FS_ERR_LIMIT;
        case VFS_ERR_IO: return MYOS_FS_ERR_IO;
        case VFS_ERR_NOT_EMPTY: return MYOS_FS_ERR_NOT_EMPTY;
        default: return MYOS_FS_ERR_OPERATION;
    }
}

static userfs_handle *get_handle(userfs_context *context, uint64_t descriptor) {
    if (descriptor == 0U || descriptor > MYOS_FS_HANDLE_LIMIT) return NULL;
    userfs_handle *handle = &context->handles[descriptor - 1U];
    return handle->in_use ? handle : NULL;
}

userfs_context *userfs_create_context(void) {
    userfs_context *context = kmalloc(sizeof(*context));
    if (context == NULL) return NULL;
    memset(context, 0, sizeof(*context));
    return context;
}

void userfs_destroy_context(userfs_context *context) {
    if (context == NULL) return;
    for (size_t i = 0; i < MYOS_FS_HANDLE_LIMIT; ++i) {
        if (context->handles[i].in_use) {
            vfs_close(&context->handles[i].file);
            memset(&context->handles[i], 0, sizeof(context->handles[i]));
        }
    }
    kfree(context);
}

static int64_t syscall_open(userfs_context *context, uint64_t address_space,
                            uint64_t user_path, uint64_t user_flags) {
    const uint32_t valid_flags = MYOS_FS_OPEN_READ | MYOS_FS_OPEN_WRITE |
                                 MYOS_FS_OPEN_CREATE | MYOS_FS_OPEN_TRUNCATE;
    if (user_flags > UINT32_MAX) return MYOS_FS_ERR_INVALID;
    uint32_t flags = (uint32_t)user_flags;
    if ((flags & ~valid_flags) != 0U ||
        (flags & (MYOS_FS_OPEN_READ | MYOS_FS_OPEN_WRITE)) == 0U ||
        ((flags & (MYOS_FS_OPEN_CREATE | MYOS_FS_OPEN_TRUNCATE)) != 0U &&
         (flags & MYOS_FS_OPEN_WRITE) == 0U)) {
        return MYOS_FS_ERR_INVALID;
    }

    char path[MYOS_FS_MAX_PATH];
    int path_rc = copy_user_path(address_space, user_path, path);
    if (path_rc != 0) return path_rc;
    size_t free_slot = MYOS_FS_HANDLE_LIMIT;
    for (size_t i = 0; i < MYOS_FS_HANDLE_LIMIT; ++i) {
        if (!context->handles[i].in_use) {
            free_slot = i;
            break;
        }
    }
    if (free_slot == MYOS_FS_HANDLE_LIMIT) return MYOS_FS_ERR_LIMIT;

    vfs_file file;
    memset(&file, 0, sizeof(file));
    int rc = vfs_open(path, &file);
    if (rc == VFS_ERR_NOT_FOUND && (flags & MYOS_FS_OPEN_CREATE) != 0U) {
        rc = vfs_create(path);
        if (rc == 0) rc = vfs_open(path, &file);
    }
    if (rc != 0) return map_vfs_error(rc);

    if (file.type != VFS_TYPE_FILE && file.type != VFS_TYPE_DIR) {
        vfs_close(&file);
        return MYOS_FS_ERR_OPERATION;
    }
    if (file.type == VFS_TYPE_DIR && (flags & MYOS_FS_OPEN_WRITE) != 0U) {
        vfs_close(&file);
        return MYOS_FS_ERR_IS_DIRECTORY;
    }
    if ((flags & MYOS_FS_OPEN_TRUNCATE) != 0U) {
        rc = vfs_truncate(&file, 0U);
        if (rc != 0) {
            vfs_close(&file);
            return map_vfs_error(rc);
        }
    }

    context->handles[free_slot].in_use = true;
    context->handles[free_slot].flags = flags;
    context->handles[free_slot].offset = 0;
    context->handles[free_slot].directory_index = 0;
    context->handles[free_slot].file = file;
    memcpy(context->handles[free_slot].path, path, strlen(path) + 1U);
    return (int64_t)(free_slot + 1U);
}

static int64_t syscall_read(userfs_context *context, uint64_t address_space,
                            uint64_t descriptor, uint64_t user_buffer,
                            uint64_t length) {
    userfs_handle *handle = get_handle(context, descriptor);
    if (handle == NULL) return MYOS_FS_ERR_BAD_HANDLE;
    if ((handle->flags & MYOS_FS_OPEN_READ) == 0U) return MYOS_FS_ERR_OPERATION;
    if (length > MYOS_FS_MAX_TRANSFER) return MYOS_FS_ERR_LIMIT;
    if (length == 0U) return 0;
    if (!vm_user_range_writable(address_space, user_buffer, length)) {
        return MYOS_FS_ERR_INVALID;
    }
    if (handle->offset > UINT64_MAX - length) return MYOS_FS_ERR_LIMIT;

    uint8_t buffer[MYOS_FS_MAX_TRANSFER];
    uint64_t got = 0;
    int rc = vfs_read(&handle->file, handle->offset, length, buffer, &got);
    if (got > length) return MYOS_FS_ERR_OPERATION;
    if (got == 0U) return rc == 0 ? 0 : map_vfs_error(rc);
    if (!user_copy_to(address_space, user_buffer, buffer, (size_t)got)) {
        return MYOS_FS_ERR_INVALID;
    }
    handle->offset += got;
    return (int64_t)got;
}

static int64_t syscall_write(userfs_context *context, uint64_t address_space,
                             uint64_t descriptor, uint64_t user_buffer,
                             uint64_t length) {
    userfs_handle *handle = get_handle(context, descriptor);
    if (handle == NULL) return MYOS_FS_ERR_BAD_HANDLE;
    if ((handle->flags & MYOS_FS_OPEN_WRITE) == 0U) return MYOS_FS_ERR_OPERATION;
    if (length > MYOS_FS_MAX_TRANSFER) return MYOS_FS_ERR_LIMIT;
    if (length == 0U) return 0;
    if (handle->offset > UINT32_MAX || length > UINT32_MAX - handle->offset) {
        return MYOS_FS_ERR_LIMIT;
    }

    uint8_t buffer[MYOS_FS_MAX_TRANSFER];
    if (!user_copy_from(address_space, buffer, user_buffer, (size_t)length)) {
        return MYOS_FS_ERR_INVALID;
    }
    uint64_t written = 0;
    int rc = vfs_write(&handle->file, handle->offset, length, buffer, &written);
    if (written > length) return MYOS_FS_ERR_OPERATION;
    handle->offset += written;
    if (rc != 0) return map_vfs_error(rc);
    return (int64_t)written;
}

static int64_t syscall_readdir(userfs_context *context, uint64_t address_space,
                               uint64_t descriptor, uint64_t user_entry) {
    userfs_handle *handle = get_handle(context, descriptor);
    if (handle == NULL) return MYOS_FS_ERR_BAD_HANDLE;
    if (handle->file.type != VFS_TYPE_DIR) return MYOS_FS_ERR_NOT_DIRECTORY;
    if (handle->directory_index > INT32_MAX) return MYOS_FS_ERR_LIMIT;
    if (!vm_user_range_writable(address_space, user_entry,
                                sizeof(myos_fs_dirent))) {
        return MYOS_FS_ERR_INVALID;
    }

    myos_fs_dirent entry;
    memset(&entry, 0, sizeof(entry));
    uint32_t type = 0;
    int rc = vfs_readdir(handle->path, (int)handle->directory_index,
                         entry.name, sizeof(entry.name), &type);
    if (rc == VFS_READDIR_EOF) return 0;
    if (rc != 0) return map_vfs_error(rc);
    entry.type = type;
    if (!user_copy_to(address_space, user_entry, &entry, sizeof(entry))) {
        return MYOS_FS_ERR_INVALID;
    }
    ++handle->directory_index;
    return 1;
}

static int64_t syscall_close(userfs_context *context, uint64_t descriptor) {
    userfs_handle *handle = get_handle(context, descriptor);
    if (handle == NULL) return MYOS_FS_ERR_BAD_HANDLE;
    vfs_close(&handle->file);
    memset(handle, 0, sizeof(*handle));
    return 0;
}

static int64_t syscall_path_operation(uint64_t address_space, uint64_t user_path,
                                      unsigned int operation) {
    char path[MYOS_FS_MAX_PATH];
    int rc = copy_user_path(address_space, user_path, path);
    if (rc != 0) return rc;
    if (operation == 0U) {
        rc = vfs_unlink(path);
    } else if (operation == 1U) {
        rc = vfs_mkdir(path);
    } else {
        rc = vfs_rmdir(path);
    }
    return rc == 0 ? 0 : map_vfs_error(rc);
}

bool userfs_handle_syscall(userfs_context *context, uint64_t number,
                           uint64_t arg1, uint64_t arg2, uint64_t arg3,
                           uint64_t address_space, int64_t *result) {
    if (context == NULL || result == NULL) return false;
    switch (number) {
        case MYOS_SYS_FS_OPEN:
            *result = syscall_open(context, address_space, arg1, arg2);
            return true;
        case MYOS_SYS_FS_READ:
            *result = syscall_read(context, address_space, arg1, arg2, arg3);
            return true;
        case MYOS_SYS_FS_WRITE:
            *result = syscall_write(context, address_space, arg1, arg2, arg3);
            return true;
        case MYOS_SYS_FS_READDIR:
            *result = syscall_readdir(context, address_space, arg1, arg2);
            return true;
        case MYOS_SYS_FS_CLOSE:
            *result = syscall_close(context, arg1);
            return true;
        case MYOS_SYS_FS_MKDIR:
            *result = syscall_path_operation(address_space, arg1, 1U);
            return true;
        case MYOS_SYS_FS_UNLINK:
            *result = syscall_path_operation(address_space, arg1, 0U);
            return true;
        case MYOS_SYS_FS_RMDIR:
            *result = syscall_path_operation(address_space, arg1, 2U);
            return true;
        default:
            return false;
    }
}
