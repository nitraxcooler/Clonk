/*
 * MyOS FAT32 driver (read-write).
 *
 * Validates a FAT32 boot sector, follows cluster chains through the FAT,
 * lists and looks up 8.3 directory entries, reads and writes file data,
 * and creates/deletes files and directories. It plugs into the VFS via
 * the `fat32_ops` vtable so the shell can use it without knowing any
 * FAT32 internals.
 */

#include "fs/fat32.h"

#include <stddef.h>
#include <stdint.h>

#include "kernel/heap.h"
#include "lib/mem.h"
#include "lib/string.h"

#define F32_EOC   0x0FFFFFF8UL /* end-of-clusterchain marker (and above) */
#define ATTR_DIR  0x10

/* What we remember about an open file so we can find it again later. */
typedef struct {
    uint32_t first_cluster;
    uint32_t parent_cluster;  /* cluster of directory holding our entry  */
    uint32_t entry_offset;    /* byte offset of entry in that directory   */
    uint32_t size;            /* cached size (files only)                 */
} fat32_fhandle;

/* These just shuffle bytes around inside a 32-byte directory entry.
 * FAT likes to scatter the cluster number across two spots, so here we are. */

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

/* Dealing with the 8.3 naming scheme. */

/* Make a human-friendly name out of the space-padded on-disk one. */
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

/* Uppercase a string. FAT treats names case-insensitively, so we compare
 * everything in caps to keep it simple. */
static void f32_upper(char *s) {
    while (*s) {
        if (*s >= 'a' && *s <= 'z') {
            *s = (char)(*s - 'a' + 'A');
        }
        ++s;
    }
}

/* Squash a normal name like "hello.txt" into the fixed 11-byte 8.3
 * form FAT wants on disk: uppercase and space-padded. */
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

/* The low-level stuff: talking to clusters and FAT entries. */

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
    return v & 0x0FFFFFFFUL; /* top 4 bits are reserved */
}

/* Write a FAT entry. FAT keeps backup copies, so update them all. */
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
        /* Preserve the reserved top nibble of the existing entry. */
        uint32_t new_val = (orig & 0xF0000000UL) | (value & 0x0FFFFFFFUL);
        memcpy(scratch + off, &new_val, 4);
        if (blockdev_write_sectors(fs->dev, fat_sec, 1, scratch) != 0) {
            return -1;
        }
    }
    return 0;
}

/* Hand out a free cluster and mark it as the end of a chain.
 * Returns the cluster, or 0 when the disk is full. */
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
        if (f32_fat_entry(fs, cl) == 0) {          /* 0 = free */
            uint32_t next = cl + 1;
            if (next >= 2 + total_clusters) next = 2;
            fs->next_cluster = next;
            f32_set_fat_entry(fs, cl, F32_EOC);
            return cl;
        }
        cl++;
        if (cl >= 2 + total_clusters) cl = 2;
    } while (cl != last);

    fs->next_cluster = 2;
    return 0;                                      /* volume full */
}

/* Walk a cluster chain and mark every cluster free. */
static int f32_free_chain(fat32_fs *fs, uint32_t first_cluster) {
    uint32_t cl = first_cluster;
    int guard = 0;

    while (cl >= 2 && cl < F32_EOC && guard++ < (1 << 20)) {
        uint32_t next = f32_fat_entry(fs, cl);
        f32_set_fat_entry(fs, cl, 0);
        if (next >= 2 && next < F32_EOC) {
            cl = next;
        } else {
            break;
        }
    }
    return 0;
}

/* Everything that walks through directories. */

/* Callbacks get the raw 32-byte entry, which directory cluster it lives
 * in, and where in that directory it is. Return non-zero to bail out. */
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
            if (cb(e, cl, dir_off + off, arg) != 0) {
                return 0;
            }
        }
        cl = f32_fat_entry(fs, cl);
        dir_off += fs->cluster_bytes;
    }
    return 0;
}

/* Grab one raw 32-byte entry at a specific offset in a directory. */
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

/* Put one raw 32-byte entry back at a specific offset in a directory. */
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

/* Figuring out paths and listing directories. */

typedef struct {
    char needle[13];              /* upper-cased name to find          */
    uint8_t found_entry[32];      /* copy of the matching entry        */
    int found;
    uint32_t parent_cluster;      /* cluster containing the match      */
    uint32_t entry_offset;        /* byte offset within the chain      */
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
    int target;     /* wanted index */
    int count;      /* current index */
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

/* Split a path like "/foo/bar.txt" into its directory ("/foo") and the
 * final name ("bar.txt"). Everything before the last slash must already
 * exist and be a real directory. */
static int f32_resolve_parent(fat32_fs *fs, const char *path,
                              uint32_t *parent_cluster,
                              char *filename, size_t filename_cap) {
    const char *p = path;
    uint32_t dir_cluster = fs->root_cluster;

    while (*p == '/') p++;
    if (*p == '\0') return -1;              /* "/" has no filename */

    /* Split at the last '/'. */
    const char *last_slash = NULL;
    for (const char *q = p; *q; q++) {
        if (*q == '/') last_slash = q;
    }

    const char *fn_start = last_slash ? last_slash + 1 : p;
    size_t fn_len = 0;
    while (fn_start[fn_len] && fn_start[fn_len] != '/' &&
           fn_len < filename_cap - 1) {
        filename[fn_len] = fn_start[fn_len];
        fn_len++;
    }
    filename[fn_len] = '\0';
    if (fn_len == 0) return -1;             /* trailing slash, no name */

    /* Walk every component before the final one. */
    p = path;
    while (*p == '/') p++;
    const char *dir_end = last_slash ? last_slash : p;

    while (p < dir_end) {
        char comp[13];
        int n = 0;
        while (p < dir_end && *p != '/' && n < 12) {
            comp[n++] = *p++;
        }
        comp[n] = '\0';
        while (p < dir_end && *p == '/') p++;
        if (n == 0 || strcmp(comp, ".") == 0) continue;
        /* ".." is not supported on write paths for now. */
        if (strcmp(comp, "..") == 0) return -1;

        f32_upper(comp);

        find_ctx fc;
        memcpy(fc.needle, comp, sizeof(fc.needle));
        fc.found = 0;
        if (f32_walk_dir(fs, dir_cluster, f32_find_cb, &fc) != 0 || !fc.found) {
            return -1;
        }
        const uint8_t *e = fc.found_entry;
        if (!(e[0x0B] & ATTR_DIR)) return -1;
        dir_cluster = f32_entry_cluster(e);
    }

    *parent_cluster = dir_cluster;
    return 0;
}

/* Find an empty spot in a directory for a new entry. Returns 1 if we
 * found one, 0 if the directory needs to grow, -1 on I/O failure. */
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

/* Make room in a directory by adding a fresh zeroed cluster to its chain. */
static int f32_grow_dir(fat32_fs *fs, uint32_t prev_cluster,
                        uint32_t *new_offset) {
    uint32_t new_cl = f32_alloc_cluster(fs);
    if (new_cl == 0) return -1;

    uint8_t *buf = kmalloc(fs->cluster_bytes);
    if (buf == NULL) {
        f32_set_fat_entry(fs, new_cl, 0);
        return -1;
    }
    memset(buf, 0, fs->cluster_bytes);
    if (f32_write_cluster(fs, new_cl, buf) != 0) {
        kfree(buf);
        f32_set_fat_entry(fs, new_cl, 0);
        return -1;
    }
    kfree(buf);

    if (prev_cluster >= 2 && prev_cluster < F32_EOC) {
        f32_set_fat_entry(fs, prev_cluster, new_cl);
    }
    *new_offset = 0;
    return 0;
}

/* These are the actual VFS callbacks FAT32 implements. */

static int f32_vfs_open(vfs_mount *m, const char *path, vfs_file *out) {
    fat32_fs *fs = (fat32_fs *)m->fs_data;
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
        while (*p && *p != '/' && n < 12) {
            comp[n++] = *p++;
        }
        comp[n] = '\0';

        if (strcmp(comp, ".") == 0 || strcmp(comp, "..") == 0) {
            continue;
        }

        f32_upper(comp); /* names on disk are always caps */

        find_ctx ctx;
        memcpy(ctx.needle, comp, sizeof(ctx.needle));
        ctx.found = 0;
        ctx.parent_cluster = 0;
        ctx.entry_offset = 0;
        f32_walk_dir(fs, dir_cluster, f32_find_cb, &ctx);
        if (!ctx.found) {
            return -1; /* component not found */
        }

        const uint8_t *e = ctx.found_entry;
        comp_cluster = f32_entry_cluster(e);
        comp_size = f32_entry_size(e);
        comp_is_dir = (e[0x0B] & ATTR_DIR) != 0;
        found_any = 1;

        last_parent = dir_cluster;   /* directory that holds this entry */
        last_off = ctx.entry_offset;
        dir_cluster = comp_cluster;
    }

    if (!found_any) {
        /* Empty path means the root directory. */
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
    if (f->type != VFS_TYPE_FILE || f->fs_handle == NULL) {
        return -1;
    }
    if (size == 0) { *got = 0; return 0; }
    if (offset >= f->size) { *got = 0; return 0; }

    /* Don't read past the end of the file — the last cluster may have
     * junk bytes in it beyond the real size. */
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

/* If a write changed the file's cluster or size, write that back to the
 * directory entry on disk so it isn't lost. */
static void f32_sync_dirent(fat32_fs *fs, fat32_fhandle *fh,
                            uint32_t orig_first, uint32_t orig_size) {
    if (fh->first_cluster == orig_first && fh->size == orig_size) {
        return; /* nothing changed */
    }
    uint8_t entry_buf[32];
    if (f32_read_dir_entry(fs, fh->parent_cluster, fh->entry_offset,
                           entry_buf) != 0) {
        return;
    }
    if (fh->first_cluster != orig_first) {
        f32_set_entry_cluster(entry_buf, fh->first_cluster);
    }
    if (fh->size != orig_size) {
        f32_set_entry_size(entry_buf, fh->size);
    }
    f32_write_dir_entry(fs, fh->parent_cluster, fh->entry_offset, entry_buf);
}

/* Writes can extend a file, which means grabbing new clusters from the
 * FAT. If you write past the end, the gap gets zero-filled because we
 * always clear fresh clusters before using them. */
static int f32_vfs_write(vfs_mount *m, vfs_file *f, uint64_t offset,
                         uint64_t size, const void *buf, uint64_t *written) {
    fat32_fs *fs = (fat32_fs *)m->fs_data;
    if (f->type != VFS_TYPE_FILE || f->fs_handle == NULL) {
        if (written) *written = 0;
        return -1;
    }
    if (size == 0) { *written = 0; return 0; }

    fat32_fhandle *fh = (fat32_fhandle *)f->fs_handle;
    const uint8_t *src = (const uint8_t *)buf;
    uint8_t cbuf[4096];
    uint64_t remaining = size;
    uint64_t pos = 0;          /* byte offset of current cluster in file */
    uint32_t cl = fh->first_cluster;
    uint32_t prev_cl = 0;      /* cluster preceding cl (for linking)     */
    uint32_t orig_first = fh->first_cluster;
    uint32_t orig_size = fh->size;
    int guard = 0;

    *written = 0;

    /* Follow the chain until we're at the cluster that holds `offset`.
     * If we run out of clusters, that's fine — we allocate below. */
    while (cl >= 2 && cl < F32_EOC &&
           pos + fs->cluster_bytes <= offset && guard++ < (1 << 20)) {
        prev_cl = cl;
        cl = f32_fat_entry(fs, cl);
        pos += fs->cluster_bytes;
    }

    uint64_t want_end = offset + size;
    while (remaining > 0 && guard++ < (1 << 20)) {
        if (cl < 2 || cl >= F32_EOC) {
            /* Ran off the end — grab a fresh cluster and link it on. */
            uint32_t new_cl = f32_alloc_cluster(fs);
            if (new_cl == 0) break;             /* volume full */

            memset(cbuf, 0, fs->cluster_bytes);
            if (f32_write_cluster(fs, new_cl, cbuf) != 0) {
                f32_set_fat_entry(fs, new_cl, 0);
                break;
            }
            if (prev_cl >= 2 && prev_cl < F32_EOC) {
                f32_set_fat_entry(fs, prev_cl, new_cl);
            } else {
                fh->first_cluster = new_cl;      /* very first cluster  */
            }
            cl = new_cl;
        } else if (f32_read_cluster(fs, cl, cbuf) != 0) {
            break;
        }

        /* Copy just the bytes of this cluster that we actually need. */
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
        f32_sync_dirent(fs, fh, orig_first, orig_size);
    }
    return (*written > 0) ? 0 : -1;
}

static int f32_vfs_readdir(vfs_mount *m, const char *path, int idx,
                           char *name, size_t name_cap, uint32_t *type) {
    fat32_fs *fs = (fat32_fs *)m->fs_data;
    if (idx < 0) {
        return -1;
    }

    /* Which directory are we listing? Root, or open the subdirectory. */
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

/* The mutating operations: create, unlink, and mkdir. */

static int f32_vfs_create(vfs_mount *m, const char *path) {
    fat32_fs *fs = (fat32_fs *)m->fs_data;

    char filename[13];
    uint32_t parent_cluster;
    if (f32_resolve_parent(fs, path, &parent_cluster,
                           filename, sizeof(filename)) != 0) {
        return -1;
    }

    /* The name must not already exist. */
    char needle[13];
    memcpy(needle, filename, sizeof(needle));
    f32_upper(needle);

    find_ctx ctx;
    memcpy(ctx.needle, needle, sizeof(ctx.needle));
    ctx.found = 0;
    if (f32_walk_dir(fs, parent_cluster, f32_find_cb, &ctx) != 0) return -1;
    if (ctx.found) return -1;

    /* Find a slot in the parent, or grow the directory if it's full. */
    uint32_t slot_off = 0;
    uint32_t prev_cl = 0;
    int r = f32_find_free_slot(fs, parent_cluster, &slot_off, &prev_cl);
    if (r == 0) {
        if (f32_grow_dir(fs, prev_cl, &slot_off) != 0) return -1;
    } else if (r < 0) {
        return -1;
    }

    /* A brand-new file starts out as just a directory entry, no data. */
    uint8_t entry[32];
    memset(entry, 0, sizeof(entry));
    f32_to_83(filename, entry);
    entry[0x0B] = 0x20;                       /* archive attribute */

    return f32_write_dir_entry(fs, parent_cluster, slot_off, entry);
}

static int f32_vfs_unlink(vfs_mount *m, const char *path) {
    fat32_fs *fs = (fat32_fs *)m->fs_data;

    char filename[13];
    uint32_t parent_cluster;
    if (f32_resolve_parent(fs, path, &parent_cluster,
                           filename, sizeof(filename)) != 0) {
        return -1;
    }

    char needle[13];
    memcpy(needle, filename, sizeof(needle));
    f32_upper(needle);

    find_ctx ctx;
    memcpy(ctx.needle, needle, sizeof(ctx.needle));
    ctx.found = 0;
    if (f32_walk_dir(fs, parent_cluster, f32_find_cb, &ctx) != 0) return -1;
    if (!ctx.found) return -1;                /* no such file */

    if (ctx.found_entry[0x0B] & ATTR_DIR) {
        return -1;                            /* can't rm a directory */
    }

    /* Free its data clusters first, then remove the directory entry. */
    uint32_t first_cl = f32_entry_cluster(ctx.found_entry);
    if (first_cl >= 2 && first_cl < F32_EOC) {
        f32_free_chain(fs, first_cl);
    }

    uint8_t entry_buf[32];
    memcpy(entry_buf, ctx.found_entry, 32);
    entry_buf[0] = 0xE5;                      /* the "deleted" marker */
    return f32_write_dir_entry(fs, ctx.parent_cluster,
                               ctx.entry_offset, entry_buf);
}

static int f32_vfs_mkdir(vfs_mount *m, const char *path) {
    fat32_fs *fs = (fat32_fs *)m->fs_data;

    char dirname[13];
    uint32_t parent_cluster;
    if (f32_resolve_parent(fs, path, &parent_cluster,
                           dirname, sizeof(dirname)) != 0) {
        return -1;
    }

    char needle[13];
    memcpy(needle, dirname, sizeof(needle));
    f32_upper(needle);

    find_ctx ctx;
    memcpy(ctx.needle, needle, sizeof(ctx.needle));
    ctx.found = 0;
    if (f32_walk_dir(fs, parent_cluster, f32_find_cb, &ctx) != 0) return -1;
    if (ctx.found) return -1;                 /* already exists */

    /* Room in the parent directory. */
    uint32_t slot_off = 0;
    uint32_t prev_cl = 0;
    int r = f32_find_free_slot(fs, parent_cluster, &slot_off, &prev_cl);
    if (r == 0) {
        if (f32_grow_dir(fs, prev_cl, &slot_off) != 0) return -1;
    } else if (r < 0) {
        return -1;
    }

    /* Grab a cluster to store the new directory's entries in. */
    uint32_t new_cl = f32_alloc_cluster(fs);
    if (new_cl == 0) return -1;

    uint8_t *dbuf = kmalloc(fs->cluster_bytes);
    if (dbuf == NULL) {
        f32_set_fat_entry(fs, new_cl, 0);
        return -1;
    }
    memset(dbuf, 0, fs->cluster_bytes);

    /* "." is this directory itself. */
    memset(dbuf, ' ', 11);
    dbuf[0] = '.';
    dbuf[0x0B] = ATTR_DIR;
    f32_set_entry_cluster(dbuf, new_cl);

    /* ".." points back at whoever owns it. */
    memset(dbuf + 32, ' ', 11);
    dbuf[32] = '.';
    dbuf[33] = '.';
    dbuf[32 + 0x0B] = ATTR_DIR;
    f32_set_entry_cluster(dbuf + 32, parent_cluster);

    if (f32_write_cluster(fs, new_cl, dbuf) != 0) {
        kfree(dbuf);
        f32_set_fat_entry(fs, new_cl, 0);
        return -1;
    }
    kfree(dbuf);

    uint8_t entry[32];
    memset(entry, 0, sizeof(entry));
    f32_to_83(dirname, entry);
    entry[0x0B] = ATTR_DIR;
    f32_set_entry_cluster(entry, new_cl);     /* directories have no size */

    if (f32_write_dir_entry(fs, parent_cluster, slot_off, entry) != 0) {
        f32_set_fat_entry(fs, new_cl, 0);
        return -1;
    }
    return 0;
}

/* Mounting: check the boot sector looks like a real FAT32 volume. */

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

    if (bps != 512) return -1;              /* our block layer only does 512 */
    if (spc == 0 || (spc & (spc - 1)) != 0) return -1; /* needs to be a power of 2 */
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
    fs->next_cluster        = 2; /* allocation scan starts at cluster 2 */
    fs->valid               = 1;
    return 0;
}

struct vfs_fs_ops fat32_ops = {
    .open    = f32_vfs_open,
    .close   = f32_vfs_close,
    .read    = f32_vfs_read,
    .write   = f32_vfs_write,
    .readdir = f32_vfs_readdir,
    .stat    = f32_vfs_stat,
    .create  = f32_vfs_create,
    .unlink  = f32_vfs_unlink,
    .mkdir   = f32_vfs_mkdir,
};
