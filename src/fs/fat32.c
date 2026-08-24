/*
 * MyOS FAT32 driver (read-only).
 *
 * Validates a FAT32 boot sector/BPB, follows cluster chains through the
 * FAT, lists/looks up 8.3 directory entries and reads file content.
 * Exposed to the VFS through the `fat32_ops` vtable so the shell can
 * mount, ls and cat without knowing about FAT32 internals.
 */

#include "fs/fat32.h"

#include <stddef.h>
#include <stdint.h>

#include "kernel/heap.h"
#include "lib/mem.h"
#include "lib/string.h"

#define F32_EOC   0x0FFFFFF8UL /* end-of-clusterchain marker (and above) */
#define ATTR_DIR  0x10

/* Small per-open handle stashed in vfs_file::fs_handle. */
typedef struct {
    uint32_t first_cluster;
} fat32_fhandle;

/* ---------- low-level sector / FAT helpers ---------- */

static int f32_read_cluster(fat32_fs *fs, uint32_t cluster, void *buf) {
    uint64_t first = fs->data_start_sector +
                     (uint64_t)(cluster - 2) * fs->sectors_per_cluster;
    return blockdev_read_sectors(fs->dev, first, fs->sectors_per_cluster, buf);
}

static uint32_t f32_fat_entry(fat32_fs *fs, uint32_t cluster) {
    uint8_t scratch[512];
    uint64_t fat_byte = (uint64_t)fs->reserved_sectors * fs->bytes_per_sector
                        + (uint64_t)cluster * 4;
    uint64_t sec = fat_byte / fs->bytes_per_sector;
    uint32_t off = (uint32_t)(fat_byte % fs->bytes_per_sector);

    if (blockdev_read_sectors(fs->dev, sec, 1, scratch) != 0) {
        return 0;
    }
    uint32_t v;
    memcpy(&v, scratch + off, 4);
    return v & 0x0FFFFFFFUL; /* top 4 bits are reserved */
}

/* ---------- directory scanning ---------- */

/* Build the printable short name ("NAME.EXT", "DIR", "." etc.). */
static void f32_format_name(const uint8_t *e, char *out, size_t cap) {
    size_t n = 0;
    int base_len = 0;
    while (base_len < 8 && e[base_len] != ' ' && e[base_len] != 0) {
        base_len++;
    }
    int ext_len = 0;
    while (ext_len < 3 && e[8 + ext_len] != ' ' && e[8 + ext_len] != 0) {
        ext_len++;
    }
    for (int i = 0; i < base_len && n + 1 < cap; ++i) {
        out[n++] = (char)e[i];
    }
    if (ext_len > 0) {
        if (n + 1 < cap) out[n++] = '.';
        for (int i = 0; i < ext_len && n + 1 < cap; ++i) {
            out[n++] = (char)e[8 + i];
        }
    }
    out[n] = '\0';
}

/* Upper-case in place (for case-insensitive 8.3 comparison). */
static void f32_upper(char *s) {
    while (*s) {
        if (*s >= 'a' && *s <= 'z') {
            *s = (char)(*s - 'a' + 'A');
        }
        ++s;
    }
}

/* Iterate the directory cluster chain starting at dir_cluster, calling
 * cb(entry, arg) for each valid entry (skipping deleted entries, LFN
 * parts and volume labels).  cb returns non-zero to stop early.
 * Returns 0 on success, -1 on I/O error. */
typedef int (*dir_cb)(const uint8_t *entry, void *arg);

static int f32_walk_dir(fat32_fs *fs, uint32_t dir_cluster, dir_cb cb,
                        void *arg) {
    uint8_t cbuf[4096];
    uint32_t cl = dir_cluster;
    int guard = 0;

    while (cl >= 2 && cl < F32_EOC && guard++ < 4096) {
        if (fs->cluster_bytes > sizeof(cbuf)) {
            return -1; /* cluster larger than our scratch buffer */
        }
        if (f32_read_cluster(fs, cl, cbuf) != 0) {
            return -1;
        }
        for (uint32_t off = 0; off + 32 <= fs->cluster_bytes; off += 32) {
            const uint8_t *e = cbuf + off;
            if (e[0] == 0x00) {
                return 0; /* logical end of directory */
            }
            if (e[0] == 0xE5) {
                continue; /* deleted entry */
            }
            uint8_t attr = e[0x0B];
            if (attr == 0x0F) {
                continue; /* long-file-name fragment */
            }
            if (attr & 0x08) {
                continue; /* volume label */
            }
            if (cb(e, arg) != 0) {
                return 0;
            }
        }
        cl = f32_fat_entry(fs, cl);
    }
    return 0;
}

static uint32_t f32_entry_cluster(const uint8_t *e) {
    uint32_t lo = e[0x1A] | ((uint32_t)e[0x1B] << 8);
    uint32_t hi = e[0x14] | ((uint32_t)e[0x15] << 8);
    return (hi << 16) | lo;
}

static uint32_t f32_entry_size(const uint8_t *e) {
    return (uint32_t)e[0x1C] | ((uint32_t)e[0x1D] << 8) |
           ((uint32_t)e[0x1E] << 16) | ((uint32_t)e[0x1F] << 24);
}

/* ---------- directory search & listing ---------- */

typedef struct {
    char needle[13];              /* upper-cased name to find */
    uint8_t found_entry[32];      /* copy of the matching entry */
    int found;
} find_ctx;

static int f32_find_cb(const uint8_t *e, void *arg) {
    find_ctx *c = (find_ctx *)arg;
    char ondisk[13];
    f32_format_name(e, ondisk, sizeof(ondisk));
    if (strcmp(c->needle, ondisk) == 0) {
        memcpy(c->found_entry, e, 32);
        c->found = 1;
        return 1;
    }
    return 0;
}

typedef struct {
    int target;     /* wanted index */
    int count;      /* current index */
    uint8_t copy[32];
    int found;
} readdir_ctx;

static int f32_readdir_cb(const uint8_t *e, void *arg) {
    readdir_ctx *c = (readdir_ctx *)arg;
    if (c->count == c->target) {
        memcpy(c->copy, e, 32);
        c->found = 1;
        return 1;
    }
    c->count++;
    return 0;
}

/* ---------- VFS ops ---------- */

static int f32_vfs_open(vfs_mount *m, const char *path, vfs_file *out) {
    fat32_fs *fs = (fat32_fs *)m->fs_data;
    uint32_t dir_cluster = fs->root_cluster;
    const char *p = path;
    uint32_t comp_cluster = 0;
    uint32_t comp_size = 0;
    int comp_is_dir = 1;
    int found_any = 0;

    while (1) {
        while (*p == '/') p++;
        if (*p == '\0') break;

        char comp[13];
        int n = 0;
        while (*p && *p != '/' && n < 12) {
            comp[n++] = *p++;
        }
        comp[n] = '\0';

        if (strcmp(comp, ".") == 0 || strcmp(comp, "..") == 0) {
            continue;
        }

        f32_upper(comp); /* case-insensitive lookup */

        find_ctx ctx;
        memcpy(ctx.needle, comp, sizeof(ctx.needle));
        ctx.found = 0;
        f32_walk_dir(fs, dir_cluster, f32_find_cb, &ctx);
        if (!ctx.found) {
            return -1; /* component not found */
        }

        const uint8_t *e = ctx.found_entry;
        comp_cluster = f32_entry_cluster(e);
        comp_size = f32_entry_size(e);
        comp_is_dir = (e[0x0B] & ATTR_DIR) != 0;
        found_any = 1;

        dir_cluster = comp_cluster;
    }

    if (!found_any) {
        /* Empty path -> root directory. */
        out->mount = m;
        out->size = 0;
        out->type = VFS_TYPE_DIR;
        out->fs_handle = NULL;
        return 0;
    }

    fat32_fhandle *fh = (fat32_fhandle *)kmalloc(sizeof(fat32_fhandle));
    if (fh == NULL) {
        return -1;
    }
    fh->first_cluster = comp_cluster;

    out->mount = m;
    out->fs_handle = fh;
    out->size = comp_is_dir ? 0 : comp_size;
    out->type = comp_is_dir ? VFS_TYPE_DIR : VFS_TYPE_FILE;
    return 0;
}

static void f32_vfs_close(vfs_mount *m, vfs_file *f) {
    (void)m;
    if (f->fs_handle != NULL) {
        kfree(f->fs_handle);
        f->fs_handle = NULL;
    }
}

static int f32_vfs_read(vfs_mount *m, vfs_file *f, uint64_t offset,
                        uint64_t size, void *buf, uint64_t *got) {
    fat32_fs *fs = (fat32_fs *)m->fs_data;
    if (f->type != VFS_TYPE_FILE || f->fs_handle == NULL) {
        return -1;
    }
    if (size == 0) { *got = 0; return 0; }
    if (offset >= f->size) { *got = 0; return 0; }

    /* Clamp the request to the declared file size so we never expose
     * slack bytes stored past EOF in the last cluster. */
    uint64_t avail = f->size - offset;
    if (size > avail) {
        size = avail;
    }

    fat32_fhandle *fh = (fat32_fhandle *)f->fs_handle;
    uint32_t cl = fh->first_cluster;
    uint64_t remaining = size;
    uint64_t chain_pos = 0; /* byte offset of current cluster in file */
    uint8_t cbuf[4096];
    int guard = 0;

    while (remaining > 0 && cl >= 2 && cl < F32_EOC && guard++ < (1 << 20)) {
        uint32_t csize = fs->cluster_bytes;
        if (csize > sizeof(cbuf)) {
            csize = sizeof(cbuf);
        }
        if (f32_read_cluster(fs, cl, cbuf) != 0) {
            break;
        }
        /* Clip to the declared file size so slack bytes past EOF in the
         * last cluster are never exposed. */
        uint64_t want_end = offset + size;
        uint64_t cluster_end = chain_pos + csize;

        uint64_t copy_start = chain_pos;
        if (copy_start < offset) copy_start = offset;
        uint64_t copy_end = cluster_end;
        if (copy_end > want_end) copy_end = want_end;

        if (copy_end > copy_start) {
            memcpy((uint8_t *)buf + (copy_start - offset),
                   cbuf + (copy_start - chain_pos),
                   (size_t)(copy_end - copy_start));
            remaining -= (copy_end - copy_start);
        }
        chain_pos = cluster_end;
        cl = f32_fat_entry(fs, cl);
    }

    *got = size - remaining;
    return 0;
}

static int f32_vfs_readdir(vfs_mount *m, const char *path, int idx,
                           char *name, size_t name_cap, uint32_t *type) {
    fat32_fs *fs = (fat32_fs *)m->fs_data;
    if (idx < 0) {
        return -1;
    }

    /* Resolve the directory cluster: root, or an opened subdirectory. */
    uint32_t dir_cluster = fs->root_cluster;
    const char *p = path;
    while (*p == '/') p++;
    if (*p != '\0') {
        vfs_file dr;
        if (f32_vfs_open(m, path, &dr) != 0) {
            return -1;
        }
        if (dr.type != VFS_TYPE_DIR) {
            vfs_close(&dr);
            return -1;
        }
        if (dr.fs_handle != NULL) {
            dir_cluster = ((fat32_fhandle *)dr.fs_handle)->first_cluster;
        } else {
            dir_cluster = fs->root_cluster;
        }
        vfs_close(&dr);
    }

    readdir_ctx ctx;
    ctx.target = idx;
    ctx.count = 0;
    ctx.found = 0;
    f32_walk_dir(fs, dir_cluster, f32_readdir_cb, &ctx);

    if (!ctx.found) {
        return -1; /* index past end */
    }
    f32_format_name(ctx.copy, name, name_cap);
    *type = (ctx.copy[0x0B] & ATTR_DIR) ? VFS_TYPE_DIR : VFS_TYPE_FILE;
    return 0;
}

static int f32_vfs_stat(vfs_mount *m, const char *path, uint64_t *size,
                        uint32_t *type) {
    vfs_file f;
    if (f32_vfs_open(m, path, &f) != 0) {
        return -1;
    }
    *size = f.size;
    *type = f.type;
    vfs_close(&f);
    return 0;
}

/* ---------- mount / BPB validation ---------- */

int fat32_mount(block_device *dev, fat32_fs *fs) {
    if (dev == NULL || fs == NULL) {
        return -1;
    }

    uint8_t bpb[512];
    if (blockdev_read_sectors(dev, 0, 1, bpb) != 0) {
        return -1;
    }
    if (bpb[510] != 0x55 || bpb[511] != 0xAA) {
        return -1;
    }

    uint16_t bps      = (uint16_t)(bpb[11] | (uint16_t)(bpb[12] << 8));
    uint8_t  spc      = bpb[13];
    uint16_t reserved = (uint16_t)(bpb[14] | (uint16_t)(bpb[15] << 8));
    uint8_t  fatc     = bpb[16];
    uint32_t tot32    = (uint32_t)bpb[32] | ((uint32_t)bpb[33] << 8) |
                        ((uint32_t)bpb[34] << 16) | ((uint32_t)bpb[35] << 24);
    uint32_t fat_sz   = (uint32_t)bpb[36] | ((uint32_t)bpb[37] << 8) |
                        ((uint32_t)bpb[38] << 16) | ((uint32_t)bpb[39] << 24);
    uint32_t root_cl  = (uint32_t)bpb[44] | ((uint32_t)bpb[45] << 8) |
                        ((uint32_t)bpb[46] << 16) | ((uint32_t)bpb[47] << 24);

    if (bps != 512) return -1;              /* our block layer is 512-byte */
    if (spc == 0 || (spc & (spc - 1)) != 0) return -1; /* must be power of 2 */
    if (reserved == 0 || fatc == 0 || fat_sz == 0 || root_cl < 2) {
        return -1;
    }
    if (memcmp(bpb + 66, "FAT32   ", 8) != 0) { /* BS_FilSysType */
        return -1;
    }

    fs->dev                 = dev;
    fs->bytes_per_sector    = bps;
    fs->sectors_per_cluster = spc;
    fs->reserved_sectors    = reserved;
    fs->fat_count           = fatc;
    fs->fat_size_sectors    = fat_sz;
    fs->root_cluster        = root_cl;
    fs->total_sectors       = tot32;
    fs->data_start_sector   = reserved + (uint32_t)fatc * fat_sz;
    fs->cluster_bytes       = (uint32_t)spc * bps;
    fs->valid               = 1;
    return 0;
}

struct vfs_fs_ops fat32_ops = {
    .open    = f32_vfs_open,
    .close   = f32_vfs_close,
    .read    = f32_vfs_read,
    .readdir = f32_vfs_readdir,
    .stat    = f32_vfs_stat,
};
