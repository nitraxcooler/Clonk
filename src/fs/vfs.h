#ifndef VFS_H
#define VFS_H

#include <stddef.h>
#include <stdint.h>

#include "kernel/blockdev.h"

#define VFS_MAX_MOUNTS     8
#define VFS_MAX_PATH       256
#define VFS_NAME_MAX       64

#define VFS_TYPE_FILE      0
#define VFS_TYPE_DIR       1

#define VFS_READDIR_EOF       1
#define VFS_ERR_INVALID      -1
#define VFS_ERR_NOT_FOUND    -2
#define VFS_ERR_NOT_DIRECTORY -3
#define VFS_ERR_IS_DIRECTORY -4
#define VFS_ERR_EXISTS       -5
#define VFS_ERR_NO_MEMORY    -6
#define VFS_ERR_NO_SPACE     -7
#define VFS_ERR_READ_ONLY    -8
#define VFS_ERR_IO           -9
#define VFS_ERR_OPERATION   -10
#define VFS_ERR_LIMIT       -11
#define VFS_ERR_NOT_EMPTY   -12

typedef struct vfs_file {
    void       *mount;
    void       *fs_handle;
    uint64_t    size;
    uint32_t    type;
} vfs_file;

typedef struct vfs_mount vfs_mount;

struct vfs_fs_ops {

    int (*open)(vfs_mount *m, const char *path, vfs_file *out);

    void (*close)(vfs_mount *m, vfs_file *f);

    int (*read)(vfs_mount *m, vfs_file *f, uint64_t offset,
                uint64_t size, void *buf, uint64_t *got);

    int (*write)(vfs_mount *m, vfs_file *f, uint64_t offset,
                 uint64_t size, const void *buf, uint64_t *written);

    int (*truncate)(vfs_mount *m, vfs_file *f, uint64_t size);

    int (*readdir)(vfs_mount *m, const char *path, int idx,
                   char *name, size_t name_cap, uint32_t *type);

    int (*stat)(vfs_mount *m, const char *path, uint64_t *size,
                uint32_t *type);

    int (*create)(vfs_mount *m, const char *path);

    int (*unlink)(vfs_mount *m, const char *path);

    int (*mkdir)(vfs_mount *m, const char *path);

    int (*rmdir)(vfs_mount *m, const char *path);
};

struct vfs_mount {
    const char       *path;
    const char       *name;
    block_device     *dev;
    struct vfs_fs_ops ops;
    void             *fs_data;
    int               in_use;
};

int      vfs_init(void);

int      vfs_do_mount(const char *mount_point, const char *fs_name,
                    block_device *dev, struct vfs_fs_ops *ops, void *fs_data);
int      vfs_unmount(const char *mount_point);

vfs_mount *vfs_find_mount(const char *path);

int      vfs_open(const char *path, vfs_file *out);
void     vfs_close(vfs_file *f);
int      vfs_read(vfs_file *f, uint64_t offset, uint64_t size,
                  void *buf, uint64_t *got);
int      vfs_write(vfs_file *f, uint64_t offset, uint64_t size,
                  const void *buf, uint64_t *written);
int      vfs_truncate(vfs_file *f, uint64_t size);
int      vfs_readdir(const char *path, int idx, char *name,
                     size_t name_cap, uint32_t *type);
int      vfs_stat(const char *path, uint64_t *size, uint32_t *type);

int      vfs_create(const char *path);
int      vfs_unlink(const char *path);
int      vfs_mkdir(const char *path);
int      vfs_rmdir(const char *path);

#endif
