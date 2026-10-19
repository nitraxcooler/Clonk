/*
 * MyOS Virtual Filesystem — mount table and dispatch.
 *
 * The VFS keeps a list of mounted filesystems and routes API calls to
 * the filesystem whose mount point best matches the path. It doesn't
 * know anything about FAT32 or any other filesystem itself.
 */

#include "fs/vfs.h"

#include "boot/limine.h"
#include "kernel/panic.h"
#include "lib/mem.h"
#include "lib/string.h"

static struct vfs_mount vfs_mounts[VFS_MAX_MOUNTS];
static bool vfs_ready = false;

int vfs_init(void) {
    for (int i = 0; i < VFS_MAX_MOUNTS; ++i) {
        vfs_mounts[i].in_use = 0;
        vfs_mounts[i].path = NULL;
        vfs_mounts[i].dev = NULL;
        vfs_mounts[i].fs_data = NULL;
    }
    vfs_ready = true;
    return 0;
}

int vfs_do_mount(const char *mount_point, const char *fs_name,
                 block_device *dev, struct vfs_fs_ops *ops, void *fs_data) {
    if (!vfs_ready || mount_point == NULL || ops == NULL || dev == NULL) {
        return -1;
    }

    /* Don't allow the same mount point twice. */
    for (int i = 0; i < VFS_MAX_MOUNTS; ++i) {
        if (vfs_mounts[i].in_use &&
            strcmp(vfs_mounts[i].path, mount_point) == 0) {
            return -1;
        }
    }

    for (int i = 0; i < VFS_MAX_MOUNTS; ++i) {
        if (!vfs_mounts[i].in_use) {
            vfs_mounts[i].path = mount_point;
            vfs_mounts[i].name = fs_name;
            vfs_mounts[i].dev = dev;
            vfs_mounts[i].ops = *ops;
            vfs_mounts[i].fs_data = fs_data;
            vfs_mounts[i].in_use = 1;
            return 0;
        }
    }
    return -1; /* no free slot in the mount table */
}

int vfs_unmount(const char *mount_point) {
    for (int i = 0; i < VFS_MAX_MOUNTS; ++i) {
        if (vfs_mounts[i].in_use &&
            strcmp(vfs_mounts[i].path, mount_point) == 0) {
            vfs_mounts[i].in_use = 0;
            vfs_mounts[i].path = NULL;
            vfs_mounts[i].dev = NULL;
            vfs_mounts[i].fs_data = NULL;
            return 0;
        }
    }
    return -1;
}

vfs_mount *vfs_find_mount(const char *path) {
    if (path == NULL) {
        return NULL;
    }

    vfs_mount *best = NULL;
    size_t best_len = 0;

    for (int i = 0; i < VFS_MAX_MOUNTS; ++i) {
        if (!vfs_mounts[i].in_use) {
            continue;
        }
        size_t mlen = strlen(vfs_mounts[i].path);

        /* Match if path equals mountpoint or path continues with '/'.
         * The root mount ("/") matches every absolute path. */
        if (strncmp(path, vfs_mounts[i].path, mlen) == 0 &&
            (mlen == 1 || path[mlen] == '\0' || path[mlen] == '/')) {
            if (mlen >= best_len) {
                best = &vfs_mounts[i];
                best_len = mlen;
            }
        }
    }
    return best;
}

static const char *vfs_relative_path(vfs_mount *mount, const char *path) {
    size_t mount_len = strlen(mount->path);
    if (mount_len == 1U) {
        return path;
    }
    return path + mount_len;
}

int vfs_open(const char *path, vfs_file *out) {
    if (path == NULL || out == NULL) {
        return VFS_ERR_INVALID;
    }
    vfs_mount *m = vfs_find_mount(path);
    if (m == NULL || m->ops.open == NULL) {
        return VFS_ERR_OPERATION;
    }
    return m->ops.open(m, vfs_relative_path(m, path), out);
}

void vfs_close(vfs_file *f) {
    if (f == NULL || f->mount == NULL) {
        return;
    }
    vfs_mount *m = (vfs_mount *)f->mount;
    if (m->ops.close != NULL) {
        m->ops.close(m, f);
    }
    f->mount = NULL;
    f->fs_handle = NULL;
    f->size = 0;
}

int vfs_read(vfs_file *f, uint64_t offset, uint64_t size,
             void *buf, uint64_t *got) {
    if (f == NULL || f->mount == NULL || buf == NULL || got == NULL) {
        return VFS_ERR_INVALID;
    }
    vfs_mount *m = (vfs_mount *)f->mount;
    if (m->ops.read == NULL) {
        return VFS_ERR_OPERATION;
    }
    return m->ops.read(m, f, offset, size, buf, got);
}

int vfs_write(vfs_file *f, uint64_t offset, uint64_t size,
              const void *buf, uint64_t *written) {
    if (f == NULL || f->mount == NULL || buf == NULL || written == NULL) {
        return VFS_ERR_INVALID;
    }
    vfs_mount *m = (vfs_mount *)f->mount;
    if (m->ops.write == NULL) {
        return VFS_ERR_OPERATION;
    }
    return m->ops.write(m, f, offset, size, buf, written);
}

int vfs_truncate(vfs_file *f, uint64_t size) {
    if (f == NULL || f->mount == NULL) {
        return VFS_ERR_INVALID;
    }
    vfs_mount *m = (vfs_mount *)f->mount;
    if (m->ops.truncate == NULL) {
        return VFS_ERR_OPERATION;
    }
    return m->ops.truncate(m, f, size);
}

int vfs_readdir(const char *path, int idx, char *name,
                size_t name_cap, uint32_t *type) {
    if (path == NULL || name == NULL || name_cap == 0U || type == NULL || idx < 0) {
        return VFS_ERR_INVALID;
    }
    vfs_mount *m = vfs_find_mount(path);
    if (m == NULL || m->ops.readdir == NULL) {
        return VFS_ERR_OPERATION;
    }
    return m->ops.readdir(m, vfs_relative_path(m, path), idx, name, name_cap, type);
}

int vfs_stat(const char *path, uint64_t *size, uint32_t *type) {
    if (path == NULL || size == NULL || type == NULL) {
        return VFS_ERR_INVALID;
    }
    vfs_mount *m = vfs_find_mount(path);
    if (m == NULL || m->ops.stat == NULL) {
        return VFS_ERR_OPERATION;
    }
    return m->ops.stat(m, vfs_relative_path(m, path), size, type);
}

int vfs_create(const char *path) {
    if (path == NULL) {
        return VFS_ERR_INVALID;
    }
    vfs_mount *m = vfs_find_mount(path);
    if (m == NULL || m->ops.create == NULL) {
        return VFS_ERR_OPERATION;
    }
    return m->ops.create(m, vfs_relative_path(m, path));
}

int vfs_unlink(const char *path) {
    if (path == NULL) {
        return VFS_ERR_INVALID;
    }
    vfs_mount *m = vfs_find_mount(path);
    if (m == NULL || m->ops.unlink == NULL) {
        return VFS_ERR_OPERATION;
    }
    return m->ops.unlink(m, vfs_relative_path(m, path));
}

int vfs_mkdir(const char *path) {
    if (path == NULL) {
        return VFS_ERR_INVALID;
    }
    vfs_mount *m = vfs_find_mount(path);
    if (m == NULL || m->ops.mkdir == NULL) {
        return VFS_ERR_OPERATION;
    }
    return m->ops.mkdir(m, vfs_relative_path(m, path));
}