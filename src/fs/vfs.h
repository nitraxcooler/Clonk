#ifndef VFS_H
#define VFS_H

#include <stddef.h>
#include <stdint.h>

#include "kernel/blockdev.h"

/*
 * MyOS Virtual Filesystem — a thin abstraction between the kernel/shell and
 * concrete filesystem implementations (FAT32, MyBread, ...).
 *
 * A filesystem registers a vfs_fs_ops vtable for a mounted block device.
 * Higher layers (shell commands) only ever talk to the VFS API below; they
 * never reach into FAT32 or block-device internals.  The VFS itself has no
 * knowledge of any specific filesystem.
 *
 * Paths are absolute ("/hello.txt").  The mount point is matched as a
 * prefix; the longest match wins.
 */

#define VFS_MAX_MOUNTS     8
#define VFS_MAX_PATH       256
#define VFS_NAME_MAX       64

#define VFS_TYPE_FILE      0
#define VFS_TYPE_DIR       1

/* A handle to an opened file/directory. */
typedef struct vfs_file {
    void       *mount;      /* vfs_mount*  */
    void       *fs_handle;  /* per-filesystem open state            */
    uint64_t    size;       /* bytes (files only)                   */
    uint32_t    type;       /* VFS_TYPE_*                           */
} vfs_file;

typedef struct vfs_mount vfs_mount;

struct vfs_fs_ops {
    /* Open `path` (absolute within this fs). Returns 0 on success and
     * fills `*out`. Non-zero on failure. */
    int (*open)(vfs_mount *m, const char *path, vfs_file *out);

    /* Release a previously opened vfs_file. */
    void (*close)(vfs_mount *m, vfs_file *f);

    /* Read up to `size` bytes starting at `offset`. Sets `*got` to the
     * number actually read (0 at end of file). */
    int (*read)(vfs_mount *m, vfs_file *f, uint64_t offset,
                uint64_t size, void *buf, uint64_t *got);

    /* List directory entries. `idx` is the zero-based entry index.
     * Returns 0 and fills `name`/`type` on success, non-zero past the
     * last entry. */
    int (*readdir)(vfs_mount *m, const char *path, int idx,
                   char *name, size_t name_cap, uint32_t *type);

    /* Stat a path: file size and type. Returns 0 on success. */
    int (*stat)(vfs_mount *m, const char *path, uint64_t *size,
                uint32_t *type);
};

struct vfs_mount {
    const char       *path;   /* mount point, e.g. "/"               */
    const char       *name;   /* filesystem label, e.g. "fat32"      */
    block_device     *dev;    /* underlying device                   */
    struct vfs_fs_ops ops;    /* filesystem operations               */
    void             *fs_data;/* filesystem-private state            */
    int               in_use;
};

/* --- VFS API (used by shell / kernel, filesystem-agnostic) --- */

int      vfs_init(void);
/* Named vfs_do_mount to avoid clashing with the `vfs_mount` type. */
int      vfs_do_mount(const char *mount_point, const char *fs_name,
                    block_device *dev, struct vfs_fs_ops *ops, void *fs_data);
int      vfs_unmount(const char *mount_point);

vfs_mount *vfs_find_mount(const char *path);

int      vfs_open(const char *path, vfs_file *out);
void     vfs_close(vfs_file *f);
int      vfs_read(vfs_file *f, uint64_t offset, uint64_t size,
                  void *buf, uint64_t *got);
int      vfs_readdir(const char *path, int idx, char *name,
                     size_t name_cap, uint32_t *type);
int      vfs_stat(const char *path, uint64_t *size, uint32_t *type);

#endif