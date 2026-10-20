
#include "fs/fat32.h"

#include <stddef.h>
#include <stdint.h>

#include "kernel/heap.h"
#include "lib/mem.h"
#include "lib/string.h"

#define F32_EOC   0x0FFFFFF8UL
#define ATTR_DIR  0x10

typedef struct {
    uint32_t first_cluster;
    uint32_t parent_cluster;
    uint32_t entry_offset;
    uint32_t size;
} fat32_fhandle;

static uint32_t f32_entry_cluster(const uint8_t *e) {
    uint32_t lo = e[0x1A] | ((uint32_t)e[0x1B] << 8);
    uint32_t hi = e[0x14] | ((uint32_t)e[0x15] << 8);
    return (hi << 16) | lo;
}

static uint32_t f32_entry_size(const uint8_t *e) {
    return (uint32_t)e[0x1C] | ((uint32_t)e[0x1D] << 8) |
           ((uint32_t)e[0x1E] << 16) | ((uint32_t)e[0x1F] << 24);
}

static void f32_set_entry_cluster(uint8_t *e, uint32_t cl) {
    e[0x1A] = (uint8_t)(cl & 0xFF);
    e[0x1B] = (uint8_t)((cl >> 8) & 0xFF);
    e[0x14] = (uint8_t)((cl >> 16) & 0xFF);
    e[0x15] = (uint8_t)((cl >> 24) & 0xFF);
}

static void f32_set_entry_size(uint8_t *e, uint32_t sz) {
    e[0x1C] = (uint8_t)(sz & 0xFF);
    e[0x1D] = (uint8_t)((sz >> 8) & 0xFF);
    e[0x1E] = (uint8_t)((sz >> 16) & 0xFF);
    e[0x1F] = (uint8_t)((sz >> 24) & 0xFF);
}

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

static void f32_upper(char *s) {
    while (*s) {
        if (*s >= 'a' && *s <= 'z') {
            *s = (char)(*s - 'a' + 'A');
        }
        ++s;
    }
}

static void f32_to_83(const char *name, uint8_t *out) {
    for (int i = 0; i < 11; i++) out[i] = ' ';

    const char *dot = NULL;
    for (const char *p = name; *p; p++) {
        if (*p == '.') dot = p;
    }

    int base_len = dot ? (int)(dot - name) : (int)strlen(name);
    if (base_len > 8) base_len = 8;
    int ext_len = dot ? (int)strlen(dot + 1) : 0;
    if (ext_len > 3) ext_len = 3;

    for (int i = 0; i < base_len; i++) {
        out[i] = (uint8_t)name[i];
        if (out[i] >= 'a' && out[i] <= 'z') out[i] -= 32;
    }
    if (dot) {
        for (int i = 0; i < ext_len; i++) {
            out[8 + i] = (uint8_t)dot[1 + i];
            if (out[8 + i] >= 'a' && out[8 + i] <= 'z') out[8 + i] -= 32;
        }
    }
}

static int f32_short_name_valid(const char *name) {
    size_t base_len = 0;
    size_t ext_len = 0;
    int in_extension = 0;
    if (name == NULL || name[0] == '\0') return 0;

    for (size_t i = 0; name[i] != '\0'; ++i) {
        char c = name[i];
        if (c == '.') {
            if (in_extension || base_len == 0U) return 0;
            in_extension = 1;
            continue;
        }
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '~')) {
            return 0;
        }
        if (in_extension) {
            if (++ext_len > 3U) return 0;
        } else if (++base_len > 8U) {
            return 0;
        }
    }
    return base_len != 0U && (!in_extension || ext_len != 0U);
}

static int f32_read_cluster(fat32_fs *fs, uint32_t cluster, void *buf) {
    uint64_t first = fs->data_start_sector +
                     (uint64_t)(cluster - 2) * fs->sectors_per_cluster;
    return blockdev_read_sectors(fs->dev, first, fs->sectors_per_cluster, buf);
}

static int f32_write_cluster(fat32_fs *fs, uint32_t cluster, const void *buf) {
    uint64_t first = fs->data_start_sector +
                     (uint64_t)(cluster - 2) * fs->sectors_per_cluster;
    return blockdev_write_sectors(fs->dev, first, fs->sectors_per_cluster, buf);
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
    return v & 0x0FFFFFFFUL;
}

static int f32_set_fat_entry(fat32_fs *fs, uint32_t cluster, uint32_t value) {
    uint8_t scratch[512];
    uint64_t fat_byte = (uint64_t)fs->reserved_sectors * fs->bytes_per_sector
                        + (uint64_t)cluster * 4;
    uint64_t sec = fat_byte / fs->bytes_per_sector;
    uint32_t off = (uint32_t)(fat_byte % fs->bytes_per_sector);

    for (uint32_t i = 0; i < fs->fat_count; i++) {
        uint64_t fat_sec = sec + (uint64_t)i * fs->fat_size_sectors;
        if (blockdev_read_sectors(fs->dev, fat_sec, 1, scratch) != 0) {
            return -1;
        }
        uint32_t orig;
        memcpy(&orig, scratch + off, 4);

        uint32_t new_val = (orig & 0xF0000000UL) | (value & 0x0FFFFFFFUL);
        memcpy(scratch + off, &new_val, 4);
        if (blockdev_write_sectors(fs->dev, fat_sec, 1, scratch) != 0) {
            return -1;
        }
    }
    return 0;
}

static uint32_t f32_alloc_cluster(fat32_fs *fs) {
    uint32_t total_clusters =
        (fs->total_sectors - fs->data_start_sector) / fs->sectors_per_cluster;
    if (total_clusters == 0) {
        return 0;
    }
    uint32_t last = fs->next_cluster;
    if (last < 2 || last >= 2 + total_clusters) {
        last = 2;
    }
    uint32_t cl = last;

    do {
        if (f32_fat_entry(fs, cl) == 0) {
            uint32_t next = cl + 1;
            if (next >= 2 + total_clusters) next = 2;
            if (f32_set_fat_entry(fs, cl, F32_EOC) != 0) return 0;
            fs->next_cluster = next;
            return cl;
        }
        cl++;
        if (cl >= 2 + total_clusters) cl = 2;
    } while (cl != last);

    fs->next_cluster = 2;
    return 0;
}

static int f32_free_chain(fat32_fs *fs, uint32_t first_cluster) {
    uint32_t cl = first_cluster;
    int guard = 0;

    while (cl >= 2 && cl < F32_EOC && guard++ < (1 << 20)) {
        uint32_t next = f32_fat_entry(fs, cl);
        if (next == 0U || f32_set_fat_entry(fs, cl, 0) != 0) return -1;
        if (next >= 2 && next < F32_EOC) {
            cl = next;
        } else {
            break;
        }
    }
    return guard >= (1 << 20) ? -1 : 0;
}

typedef int (*dir_cb)(const uint8_t *entry, uint32_t dir_cluster,
                      uint32_t entry_off, void *arg);

static int f32_walk_dir(fat32_fs *fs, uint32_t dir_cluster, dir_cb cb,
                        void *arg) {
    uint8_t cbuf[4096];
    uint32_t cl = dir_cluster;
    uint32_t dir_off = 0;
    int guard = 0;

    while (cl >= 2 && cl < F32_EOC && guard++ < 4096) {
        if (fs->cluster_bytes > sizeof(cbuf)) {
            return -1;
        }
        if (f32_read_cluster(fs, cl, cbuf) != 0) {
            return -1;
        }
        for (uint32_t off = 0; off + 32 <= fs->cluster_bytes; off += 32) {
            const uint8_t *e = cbuf + off;
            if (e[0] == 0x00) {
                return 0;
            }
            if (e[0] == 0xE5) {
                continue;
            }
            uint8_t attr = e[0x0B];
            if (attr == 0x0F) {
                continue;
            }
            if (attr & 0x08) {
                continue;
            }
            if (cb(e, cl, dir_off + off, arg) != 0) {
                return 0;
            }
        }
        cl = f32_fat_entry(fs, cl);
        dir_off += fs->cluster_bytes;
    }
    return 0;
}

static int f32_read_dir_entry(fat32_fs *fs, uint32_t dir_cluster,
                              uint32_t entry_off, uint8_t *out) {
    uint8_t cbuf[4096];
    if (fs->cluster_bytes > sizeof(cbuf)) return -1;

    uint32_t cl = dir_cluster;
    uint32_t skip = entry_off / fs->cluster_bytes;
    for (uint32_t i = 0; i < skip; i++) {
        cl = f32_fat_entry(fs, cl);
        if (cl < 2 || cl >= F32_EOC) return -1;
    }
    if (cl < 2 || cl >= F32_EOC) return -1;
    if (f32_read_cluster(fs, cl, cbuf) != 0) return -1;
    memcpy(out, cbuf + (entry_off % fs->cluster_bytes), 32);
    return 0;
}

static int f32_write_dir_entry(fat32_fs *fs, uint32_t dir_cluster,
                               uint32_t entry_off, const uint8_t *entry) {
    uint8_t cbuf[4096];
    if (fs->cluster_bytes > sizeof(cbuf)) return -1;

    uint32_t cl = dir_cluster;
    uint32_t skip = entry_off / fs->cluster_bytes;
    for (uint32_t i = 0; i < skip; i++) {
        cl = f32_fat_entry(fs, cl);
        if (cl < 2 || cl >= F32_EOC) return -1;
    }
    if (cl < 2 || cl >= F32_EOC) return -1;
    if (f32_read_cluster(fs, cl, cbuf) != 0) return -1;
    memcpy(cbuf + (entry_off % fs->cluster_bytes), entry, 32);
    return f32_write_cluster(fs, cl, cbuf);
}

typedef struct {
    char needle[13];
    uint8_t found_entry[32];
    int found;
    uint32_t parent_cluster;
    uint32_t entry_offset;
} find_ctx;

static int f32_find_cb(const uint8_t *e, uint32_t dir_cluster,
                       uint32_t entry_off, void *arg) {
    find_ctx *c = (find_ctx *)arg;
    char ondisk[13];
    f32_format_name(e, ondisk, sizeof(ondisk));
    if (strcmp(c->needle, ondisk) == 0) {
        memcpy(c->found_entry, e, 32);
        c->found = 1;
        c->parent_cluster = dir_cluster;
        c->entry_offset = entry_off;
        return 1;
    }
    return 0;
}

typedef struct {
    int target;
    int count;
    uint8_t copy[32];
    int found;
} readdir_ctx;

static int f32_readdir_cb(const uint8_t *e, uint32_t dir_cluster,
                          uint32_t entry_off, void *arg) {
    (void)dir_cluster;
    (void)entry_off;
    readdir_ctx *c = (readdir_ctx *)arg;
    if (c->count == c->target) {
        memcpy(c->copy, e, 32);
        c->found = 1;
        return 1;
    }
    c->count++;
    return 0;
}

static int f32_resolve_parent(fat32_fs *fs, const char *path,
                              uint32_t *parent_cluster,
                              char *filename, size_t filename_cap) {
    if (fs == NULL || path == NULL || parent_cluster == NULL ||
        filename == NULL || filename_cap < 2U) {
        return VFS_ERR_INVALID;
    }
    const char *p = path;
    uint32_t dir_cluster = fs->root_cluster;

    while (*p == '/') p++;
    if (*p == '\0') return VFS_ERR_INVALID;

    const char *last_slash = NULL;
    for (const char *q = p; *q; q++) {
        if (*q == '/') last_slash = q;
    }

    const char *fn_start = last_slash ? last_slash + 1 : p;
    size_t fn_len = 0;
    while (fn_start[fn_len] != '\0' && fn_len < filename_cap - 1U) {
        filename[fn_len] = fn_start[fn_len];
        fn_len++;
    }
    filename[fn_len] = '\0';
    if (fn_start[fn_len] != '\0' || fn_len == 0U ||
        !f32_short_name_valid(filename)) {
        return VFS_ERR_INVALID;
    }

    p = path;
    while (*p == '/') p++;
    const char *dir_end = last_slash ? last_slash : p;

    while (p < dir_end) {
        char comp[13];
        int n = 0;
        while (p < dir_end && *p != '/' && n < 12) {
            comp[n++] = *p++;
        }
        if (p < dir_end && *p != '/') return VFS_ERR_INVALID;
        comp[n] = '\0';
        while (p < dir_end && *p == '/') p++;
        if (n == 0 || strcmp(comp, ".") == 0) continue;

        if (strcmp(comp, "..") == 0 || !f32_short_name_valid(comp)) {
            return VFS_ERR_INVALID;
        }

        f32_upper(comp);

        find_ctx fc;
        memcpy(fc.needle, comp, sizeof(fc.needle));
        fc.found = 0;
        if (f32_walk_dir(fs, dir_cluster, f32_find_cb, &fc) != 0) return VFS_ERR_IO;
        if (!fc.found) return VFS_ERR_NOT_FOUND;
        const uint8_t *e = fc.found_entry;
        if (!(e[0x0B] & ATTR_DIR)) return VFS_ERR_NOT_DIRECTORY;
        dir_cluster = f32_entry_cluster(e);
    }

    *parent_cluster = dir_cluster;
    return 0;
}

static int f32_find_free_slot(fat32_fs *fs, uint32_t dir_cluster,
                              uint32_t *out_offset, uint32_t *prev_cluster) {
    uint8_t cbuf[4096];
    uint32_t cl = dir_cluster;
    uint32_t dir_off = 0;
    uint32_t last = 0;

    while (cl >= 2 && cl < F32_EOC) {
        if (fs->cluster_bytes > sizeof(cbuf)) return -1;
        if (f32_read_cluster(fs, cl, cbuf) != 0) return -1;

        for (uint32_t off = 0; off + 32 <= fs->cluster_bytes; off += 32) {
            const uint8_t *e = cbuf + off;
            if (e[0] == 0x00 || e[0] == 0xE5) {
                *out_offset = dir_off + off;
                *prev_cluster = last;
                return 1;
            }
        }
        last = cl;
        dir_off += fs->cluster_bytes;
        cl = f32_fat_entry(fs, cl);
    }

    *prev_cluster = last;
    return 0;
}

static int f32_grow_dir(fat32_fs *fs, uint32_t prev_cluster,
                        uint32_t *new_offset) {
    uint32_t new_cl = f32_alloc_cluster(fs);
    if (new_cl == 0) return VFS_ERR_NO_SPACE;

    uint8_t *buf = kmalloc(fs->cluster_bytes);
    if (buf == NULL) {
        f32_set_fat_entry(fs, new_cl, 0);
        return VFS_ERR_NO_MEMORY;
    }
    memset(buf, 0, fs->cluster_bytes);
    if (f32_write_cluster(fs, new_cl, buf) != 0) {
        kfree(buf);
        f32_set_fat_entry(fs, new_cl, 0);
        return VFS_ERR_IO;
    }
    kfree(buf);

    if (prev_cluster >= 2 && prev_cluster < F32_EOC) {
        if (f32_set_fat_entry(fs, prev_cluster, new_cl) != 0) {
            f32_set_fat_entry(fs, new_cl, 0);
            return VFS_ERR_IO;
        }
    }
    *new_offset = 0;
    return 0;
}

static int f32_vfs_open(vfs_mount *m, const char *path, vfs_file *out) {
    if (m == NULL || path == NULL || out == NULL) return VFS_ERR_INVALID;
    fat32_fs *fs = (fat32_fs *)m->fs_data;
    if (fs == NULL || !fs->valid) return VFS_ERR_OPERATION;
    uint32_t dir_cluster = fs->root_cluster;
    const char *p = path;
    uint32_t comp_cluster = 0;
    uint32_t comp_size = 0;
    int comp_is_dir = 1;
    int found_any = 0;
    uint32_t last_parent = 0;
    uint32_t last_off = 0;

    while (1) {
        while (*p == '/') p++;
        if (*p == '\0') break;

        char comp[13];
        int n = 0;
        while (*p && *p != '/') {
            if (n >= 12) return VFS_ERR_INVALID;
            comp[n++] = *p++;
        }
        comp[n] = '\0';

        if (strcmp(comp, ".") == 0 || strcmp(comp, "..") == 0) {
            continue;
        }
        if (!f32_short_name_valid(comp)) return VFS_ERR_INVALID;

        f32_upper(comp);

        find_ctx ctx;
        memcpy(ctx.needle, comp, sizeof(ctx.needle));
        ctx.found = 0;
        ctx.parent_cluster = 0;
        ctx.entry_offset = 0;
        if (f32_walk_dir(fs, dir_cluster, f32_find_cb, &ctx) != 0) {
            return VFS_ERR_IO;
        }
        if (!ctx.found) {
            return VFS_ERR_NOT_FOUND;
        }

        const uint8_t *e = ctx.found_entry;
        comp_cluster = f32_entry_cluster(e);
        comp_size = f32_entry_size(e);
        comp_is_dir = (e[0x0B] & ATTR_DIR) != 0;
        found_any = 1;

        last_parent = dir_cluster;
        last_off = ctx.entry_offset;
        const char *next = p;
        while (*next == '/') ++next;
        if (*next != '\0' && !comp_is_dir) return VFS_ERR_NOT_DIRECTORY;
        dir_cluster = comp_cluster;
    }

    if (!found_any) {

        out->mount = m;
        out->size = 0;
        out->type = VFS_TYPE_DIR;
        out->fs_handle = NULL;
        return 0;
    }

    fat32_fhandle *fh = (fat32_fhandle *)kmalloc(sizeof(fat32_fhandle));
    if (fh == NULL) {
        return VFS_ERR_NO_MEMORY;
    }
    fh->first_cluster = comp_cluster;
    fh->parent_cluster = last_parent;
    fh->entry_offset = last_off;
    fh->size = comp_is_dir ? 0 : comp_size;

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
    if (f == NULL || buf == NULL || got == NULL) return VFS_ERR_INVALID;
    *got = 0;
    if (f->type != VFS_TYPE_FILE) return VFS_ERR_IS_DIRECTORY;
    if (f->fs_handle == NULL || fs == NULL) {
        return VFS_ERR_INVALID;
    }
    if (size == 0) return 0;
    if (offset >= f->size) return 0;

    uint64_t avail = f->size - offset;
    if (size > avail) {
        size = avail;
    }

    fat32_fhandle *fh = (fat32_fhandle *)f->fs_handle;
    uint32_t cl = fh->first_cluster;
    uint64_t remaining = size;
    uint64_t chain_pos = 0;
    uint8_t cbuf[4096];
    int guard = 0;

    while (remaining > 0 && cl >= 2 && cl < F32_EOC && guard++ < (1 << 20)) {
        uint32_t csize = fs->cluster_bytes;
        if (csize > sizeof(cbuf)) {
            return VFS_ERR_OPERATION;
        }
        if (f32_read_cluster(fs, cl, cbuf) != 0) {
            break;
        }
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
    return remaining == 0U ? 0 : VFS_ERR_IO;
}

static int f32_sync_dirent(fat32_fs *fs, fat32_fhandle *fh,
                           uint32_t orig_first, uint32_t orig_size) {
    if (fh->first_cluster == orig_first && fh->size == orig_size) {
        return 0;
    }
    uint8_t entry_buf[32];
    if (f32_read_dir_entry(fs, fh->parent_cluster, fh->entry_offset,
                           entry_buf) != 0) return VFS_ERR_IO;
    if (fh->first_cluster != orig_first) {
        f32_set_entry_cluster(entry_buf, fh->first_cluster);
    }
    if (fh->size != orig_size) {
        f32_set_entry_size(entry_buf, fh->size);
    }
    return f32_write_dir_entry(fs, fh->parent_cluster, fh->entry_offset,
                               entry_buf) == 0 ? 0 : VFS_ERR_IO;
}

static int f32_vfs_write(vfs_mount *m, vfs_file *f, uint64_t offset,
                         uint64_t size, const void *buf, uint64_t *written) {
    fat32_fs *fs = (fat32_fs *)m->fs_data;
    if (f == NULL || written == NULL || (size != 0U && buf == NULL)) {
        return VFS_ERR_INVALID;
    }
    *written = 0;
    if (f->type != VFS_TYPE_FILE) {
        return VFS_ERR_IS_DIRECTORY;
    }
    if (f->fs_handle == NULL || fs == NULL) {
        return VFS_ERR_INVALID;
    }
    if (fs->dev == NULL || fs->dev->write == NULL) {
        return VFS_ERR_READ_ONLY;
    }
    if (offset > UINT32_MAX || size > UINT32_MAX - offset) {
        return VFS_ERR_LIMIT;
    }
    if (size == 0U) {
        return 0;
    }
    if (fs->cluster_bytes > 4096U) {
        return VFS_ERR_OPERATION;
    }
    if (f->type != VFS_TYPE_FILE || f->fs_handle == NULL) return VFS_ERR_INVALID;

    fat32_fhandle *fh = (fat32_fhandle *)f->fs_handle;
    const uint8_t *src = (const uint8_t *)buf;
    uint8_t cbuf[4096];
    uint64_t remaining = size;
    uint64_t pos = 0;
    uint32_t cl = fh->first_cluster;
    uint32_t prev_cl = 0;
    uint32_t orig_first = fh->first_cluster;
    uint32_t orig_size = fh->size;
    int guard = 0;

    *written = 0;

    while (cl >= 2 && cl < F32_EOC &&
           pos + fs->cluster_bytes <= offset && guard++ < (1 << 20)) {
        prev_cl = cl;
        cl = f32_fat_entry(fs, cl);
        pos += fs->cluster_bytes;
    }

    uint64_t want_end = offset + size;
    while (remaining > 0 && guard++ < (1 << 20)) {
        if (cl < 2 || cl >= F32_EOC) {

            uint32_t new_cl = f32_alloc_cluster(fs);
            if (new_cl == 0) break;

            memset(cbuf, 0, fs->cluster_bytes);
            if (f32_write_cluster(fs, new_cl, cbuf) != 0) {
                f32_set_fat_entry(fs, new_cl, 0);
                break;
            }
            if (prev_cl >= 2 && prev_cl < F32_EOC) {
                f32_set_fat_entry(fs, prev_cl, new_cl);
            } else {
                fh->first_cluster = new_cl;
            }
            cl = new_cl;
        } else if (f32_read_cluster(fs, cl, cbuf) != 0) {
            break;
        }

        uint64_t cluster_end = pos + fs->cluster_bytes;
        uint64_t copy_start = pos < offset ? offset : pos;
        uint64_t copy_end = cluster_end < want_end ? cluster_end : want_end;

        if (copy_end > copy_start) {
            memcpy(cbuf + (copy_start - pos),
                   src + (copy_start - offset),
                   (size_t)(copy_end - copy_start));
            uint64_t done = copy_end - copy_start;
            remaining -= done;
            *written += done;

            if (f32_write_cluster(fs, cl, cbuf) != 0) break;
        }

        prev_cl = cl;
        pos += fs->cluster_bytes;
        cl = f32_fat_entry(fs, cl);
    }

    if (*written > 0) {
        uint64_t new_size = offset + *written;
        if (new_size > fh->size) {
            fh->size = (uint32_t)new_size;
            f->size = new_size;
        }
        if (f32_sync_dirent(fs, fh, orig_first, orig_size) != 0) {
            return VFS_ERR_IO;
        }
    }
    return (*written > 0) ? 0 : VFS_ERR_IO;
}

static int f32_vfs_truncate(vfs_mount *m, vfs_file *f, uint64_t size) {
    if (m == NULL || f == NULL || f->fs_handle == NULL) {
        return VFS_ERR_INVALID;
    }
    if (f->type != VFS_TYPE_FILE) return VFS_ERR_IS_DIRECTORY;
    if (size != 0U) return VFS_ERR_LIMIT;

    fat32_fs *fs = (fat32_fs *)m->fs_data;
    if (fs == NULL || fs->dev == NULL) return VFS_ERR_OPERATION;
    if (fs->dev->write == NULL) return VFS_ERR_READ_ONLY;

    fat32_fhandle *fh = (fat32_fhandle *)f->fs_handle;
    uint32_t old_cluster = fh->first_cluster;
    if (old_cluster < 2U || old_cluster >= F32_EOC) {
        fh->first_cluster = 0;
        fh->size = 0;
        f->size = 0;
        return 0;
    }

    uint8_t entry[32];
    if (f32_read_dir_entry(fs, fh->parent_cluster, fh->entry_offset, entry) != 0) {
        return VFS_ERR_IO;
    }
    f32_set_entry_cluster(entry, 0);
    f32_set_entry_size(entry, 0);
    if (f32_write_dir_entry(fs, fh->parent_cluster, fh->entry_offset, entry) != 0) {
        return VFS_ERR_IO;
    }

    fh->first_cluster = 0;
    fh->size = 0;
    f->size = 0;
    return f32_free_chain(fs, old_cluster) == 0 ? 0 : VFS_ERR_IO;
}

static int f32_vfs_readdir(vfs_mount *m, const char *path, int idx,
                           char *name, size_t name_cap, uint32_t *type) {
    if (m == NULL || path == NULL || name == NULL || name_cap == 0U ||
        type == NULL || idx < 0) {
        return VFS_ERR_INVALID;
    }
    fat32_fs *fs = (fat32_fs *)m->fs_data;
    if (fs == NULL || !fs->valid) return VFS_ERR_OPERATION;

    uint32_t dir_cluster = fs->root_cluster;
    const char *p = path;
    while (*p == '/') p++;
    if (*p != '\0') {
        vfs_file dr;
        int rc = f32_vfs_open(m, path, &dr);
        if (rc != 0) return rc;
        if (dr.type != VFS_TYPE_DIR) {
            vfs_close(&dr);
            return VFS_ERR_NOT_DIRECTORY;
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
    if (f32_walk_dir(fs, dir_cluster, f32_readdir_cb, &ctx) != 0) {
        return VFS_ERR_IO;
    }

    if (!ctx.found) {
        return VFS_READDIR_EOF;
    }
    f32_format_name(ctx.copy, name, name_cap);
    *type = (ctx.copy[0x0B] & ATTR_DIR) ? VFS_TYPE_DIR : VFS_TYPE_FILE;
    return 0;
}

static int f32_vfs_stat(vfs_mount *m, const char *path, uint64_t *size,
                        uint32_t *type) {
    vfs_file f;
    int rc = f32_vfs_open(m, path, &f);
    if (rc != 0) return rc;
    *size = f.size;
    *type = f.type;
    vfs_close(&f);
    return 0;
}

static int f32_vfs_create(vfs_mount *m, const char *path) {
    if (m == NULL || path == NULL) return VFS_ERR_INVALID;
    fat32_fs *fs = (fat32_fs *)m->fs_data;
    if (fs == NULL || fs->dev == NULL) return VFS_ERR_OPERATION;
    if (fs->dev->write == NULL) return VFS_ERR_READ_ONLY;

    char filename[13];
    uint32_t parent_cluster;
    int rc = f32_resolve_parent(fs, path, &parent_cluster,
                                filename, sizeof(filename));
    if (rc != 0) return rc;

    char needle[13];
    memcpy(needle, filename, sizeof(needle));
    f32_upper(needle);

    find_ctx ctx;
    memcpy(ctx.needle, needle, sizeof(ctx.needle));
    ctx.found = 0;
    if (f32_walk_dir(fs, parent_cluster, f32_find_cb, &ctx) != 0) return VFS_ERR_IO;
    if (ctx.found) return VFS_ERR_EXISTS;

    uint32_t slot_off = 0;
    uint32_t prev_cl = 0;
    int r = f32_find_free_slot(fs, parent_cluster, &slot_off, &prev_cl);
    if (r == 0) {
        rc = f32_grow_dir(fs, prev_cl, &slot_off);
        if (rc != 0) return rc;
    } else if (r < 0) {
        return VFS_ERR_IO;
    }

    uint8_t entry[32];
    memset(entry, 0, sizeof(entry));
    f32_to_83(filename, entry);
    entry[0x0B] = 0x20;

    return f32_write_dir_entry(fs, parent_cluster, slot_off, entry) == 0
               ? 0 : VFS_ERR_IO;
}

static int f32_vfs_unlink(vfs_mount *m, const char *path) {
    if (m == NULL || path == NULL) return VFS_ERR_INVALID;
    fat32_fs *fs = (fat32_fs *)m->fs_data;
    if (fs == NULL || fs->dev == NULL) return VFS_ERR_OPERATION;
    if (fs->dev->write == NULL) return VFS_ERR_READ_ONLY;

    char filename[13];
    uint32_t parent_cluster;
    int rc = f32_resolve_parent(fs, path, &parent_cluster,
                                filename, sizeof(filename));
    if (rc != 0) return rc;

    char needle[13];
    memcpy(needle, filename, sizeof(needle));
    f32_upper(needle);

    find_ctx ctx;
    memcpy(ctx.needle, needle, sizeof(ctx.needle));
    ctx.found = 0;
    if (f32_walk_dir(fs, parent_cluster, f32_find_cb, &ctx) != 0) return VFS_ERR_IO;
    if (!ctx.found) return VFS_ERR_NOT_FOUND;

    if (ctx.found_entry[0x0B] & ATTR_DIR) {
        return VFS_ERR_IS_DIRECTORY;
    }

    uint32_t first_cl = f32_entry_cluster(ctx.found_entry);
    uint8_t entry_buf[32];
    memcpy(entry_buf, ctx.found_entry, 32);
    entry_buf[0] = 0xE5;
    if (f32_write_dir_entry(fs, ctx.parent_cluster,
                            ctx.entry_offset, entry_buf) != 0) {
        return VFS_ERR_IO;
    }
    if (first_cl >= 2 && first_cl < F32_EOC &&
        f32_free_chain(fs, first_cl) != 0) {
        return VFS_ERR_IO;
    }
    return 0;
}

typedef struct {
    int has_entries;
} fat32_dir_empty_ctx;

static int f32_check_directory_empty(const uint8_t *entry, uint32_t dir_cluster,
                                     uint32_t entry_offset, void *arg) {
    (void)dir_cluster;
    (void)entry_offset;
    fat32_dir_empty_ctx *ctx = (fat32_dir_empty_ctx *)arg;
    if (entry[0] == '.' && (entry[1] == ' ' || entry[1] == '.')) {
        return 0;
    }
    ctx->has_entries = 1;
    return 1;
}

static int f32_vfs_rmdir(vfs_mount *m, const char *path) {
    if (m == NULL || path == NULL) return VFS_ERR_INVALID;
    fat32_fs *fs = (fat32_fs *)m->fs_data;
    if (fs == NULL || fs->dev == NULL) return VFS_ERR_OPERATION;
    if (fs->dev->write == NULL) return VFS_ERR_READ_ONLY;

    char dirname[13];
    uint32_t parent_cluster;
    int rc = f32_resolve_parent(fs, path, &parent_cluster,
                                dirname, sizeof(dirname));
    if (rc != 0) return rc;

    char needle[13];
    memcpy(needle, dirname, sizeof(needle));
    f32_upper(needle);

    find_ctx ctx;
    memcpy(ctx.needle, needle, sizeof(ctx.needle));
    ctx.found = 0;
    if (f32_walk_dir(fs, parent_cluster, f32_find_cb, &ctx) != 0) return VFS_ERR_IO;
    if (!ctx.found) return VFS_ERR_NOT_FOUND;
    if ((ctx.found_entry[0x0B] & ATTR_DIR) == 0U) return VFS_ERR_NOT_DIRECTORY;

    uint32_t first_cluster = f32_entry_cluster(ctx.found_entry);
    if (first_cluster < 2U || first_cluster >= F32_EOC) return VFS_ERR_IO;
    fat32_dir_empty_ctx empty = { .has_entries = 0 };
    if (f32_walk_dir(fs, first_cluster, f32_check_directory_empty, &empty) != 0) {
        return VFS_ERR_IO;
    }
    if (empty.has_entries) return VFS_ERR_NOT_EMPTY;

    uint8_t entry_buf[32];
    memcpy(entry_buf, ctx.found_entry, sizeof(entry_buf));
    entry_buf[0] = 0xE5;
    if (f32_write_dir_entry(fs, ctx.parent_cluster,
                            ctx.entry_offset, entry_buf) != 0) {
        return VFS_ERR_IO;
    }
    return f32_free_chain(fs, first_cluster) == 0 ? 0 : VFS_ERR_IO;
}

static int f32_vfs_mkdir(vfs_mount *m, const char *path) {
    if (m == NULL || path == NULL) return VFS_ERR_INVALID;
    fat32_fs *fs = (fat32_fs *)m->fs_data;
    if (fs == NULL || fs->dev == NULL) return VFS_ERR_OPERATION;
    if (fs->dev->write == NULL) return VFS_ERR_READ_ONLY;

    char dirname[13];
    uint32_t parent_cluster;
    int rc = f32_resolve_parent(fs, path, &parent_cluster,
                                dirname, sizeof(dirname));
    if (rc != 0) return rc;

    char needle[13];
    memcpy(needle, dirname, sizeof(needle));
    f32_upper(needle);

    find_ctx ctx;
    memcpy(ctx.needle, needle, sizeof(ctx.needle));
    ctx.found = 0;
    if (f32_walk_dir(fs, parent_cluster, f32_find_cb, &ctx) != 0) return VFS_ERR_IO;
    if (ctx.found) return VFS_ERR_EXISTS;

    uint32_t slot_off = 0;
    uint32_t prev_cl = 0;
    int r = f32_find_free_slot(fs, parent_cluster, &slot_off, &prev_cl);
    if (r == 0) {
        rc = f32_grow_dir(fs, prev_cl, &slot_off);
        if (rc != 0) return rc;
    } else if (r < 0) {
        return VFS_ERR_IO;
    }

    uint32_t new_cl = f32_alloc_cluster(fs);
    if (new_cl == 0) return VFS_ERR_NO_SPACE;

    uint8_t *dbuf = kmalloc(fs->cluster_bytes);
    if (dbuf == NULL) {
        f32_set_fat_entry(fs, new_cl, 0);
        return VFS_ERR_NO_MEMORY;
    }
    memset(dbuf, 0, fs->cluster_bytes);

    memset(dbuf, ' ', 11);
    dbuf[0] = '.';
    dbuf[0x0B] = ATTR_DIR;
    f32_set_entry_cluster(dbuf, new_cl);

    memset(dbuf + 32, ' ', 11);
    dbuf[32] = '.';
    dbuf[33] = '.';
    dbuf[32 + 0x0B] = ATTR_DIR;
    f32_set_entry_cluster(dbuf + 32, parent_cluster);

    if (f32_write_cluster(fs, new_cl, dbuf) != 0) {
        kfree(dbuf);
        f32_set_fat_entry(fs, new_cl, 0);
        return VFS_ERR_IO;
    }
    kfree(dbuf);

    uint8_t entry[32];
    memset(entry, 0, sizeof(entry));
    f32_to_83(dirname, entry);
    entry[0x0B] = ATTR_DIR;
    f32_set_entry_cluster(entry, new_cl);

    if (f32_write_dir_entry(fs, parent_cluster, slot_off, entry) != 0) {
        f32_set_fat_entry(fs, new_cl, 0);
        return VFS_ERR_IO;
    }
    return 0;
}

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

    if (bps != 512 || bps != blockdev_sector_size(dev)) {
        return -1;
    }
    if (spc == 0 || (spc & (spc - 1)) != 0) return -1;
    if (reserved == 0 || fatc == 0 || fat_sz == 0 || root_cl < 2) {
        return -1;
    }
    if (memcmp(bpb + 82, "FAT32   ", 8) != 0) {
        return -1;
    }

    uint64_t data_start = (uint64_t)reserved + (uint64_t)fatc * fat_sz;
    if (tot32 == 0U || tot32 > dev->sector_count || data_start >= tot32) {
        return -1;
    }
    uint64_t cluster_count = (tot32 - data_start) / spc;
    uint64_t fat_entries = (uint64_t)fat_sz * bps / 4U;
    if (cluster_count == 0U || root_cl >= cluster_count + 2U ||
        fat_entries < cluster_count + 2U) {
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
    fs->data_start_sector   = (uint32_t)data_start;
    fs->cluster_bytes       = (uint32_t)spc * bps;
    fs->next_cluster        = 2;
    fs->valid               = 1;
    return 0;
}

struct vfs_fs_ops fat32_ops = {
    .open    = f32_vfs_open,
    .close   = f32_vfs_close,
    .read    = f32_vfs_read,
    .write   = f32_vfs_write,
    .truncate = f32_vfs_truncate,
    .readdir = f32_vfs_readdir,
    .stat    = f32_vfs_stat,
    .create  = f32_vfs_create,
    .unlink  = f32_vfs_unlink,
    .mkdir   = f32_vfs_mkdir,
    .rmdir   = f32_vfs_rmdir,
};
