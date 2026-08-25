/*
 * MyOS kernel heap.
 *
 * The heap lives in a reserved region of high-half virtual memory. Only
 * the part that's actually been handed physical pages is mapped; the rest
 * of the arena is just reserved address space.
 *
 * Mapped memory is split into blocks. Each block has a small header
 * (size + magic) and a footer (a copy of the size tag). The footer lets
 * us find the previous block in O(1) when freeing.
 *
 * Free blocks are linked together in a doubly-linked list. Allocation is
 * first-fit: we take the first block big enough, and split it if there's
 * a lot of leftover space. Out-of-memory returns NULL — the allocator
 * never silently corrupts memory.
 */

#include "kernel/heap.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kernel/pmm.h"
#include "kernel/vmm.h"
#include "lib/mem.h"
#include "kernel/panic.h"

#define HEAP_PAGE_SIZE PMM_PAGE_SIZE

/* The heap lives in the high half, above the kernel and the HHDM map. */
#define HEAP_VIRT_BASE  0xffffffffc0000000ULL
#define HEAP_INIT_SIZE  0x10000ULL         /* 64 KiB mapped at init          */

/* Largest payload size we can turn into a block without integer overflow. */
#define HEAP_MAX_SIZE ((size_t)-1 - 256UL)

#define HEAP_LINK_SIZE 16UL                /* free-list pointers in payload  */

typedef struct heap_block {
    uint64_t size;   /* total block size; low bit set when the block is used */
    uint64_t magic;  /* HEAP_MAGIC on used blocks, 0 on free blocks          */
} heap_block;

static uint64_t heap_mapped_bytes = 0;
static uint64_t heap_used_bytes   = 0;   /* payload bytes held by live allocs */
static uint64_t heap_free_total   = 0;   /* total bytes in free blocks        */
static uint64_t heap_alloc_count  = 0;   /* outstanding allocations           */
static uint64_t heap_free_calls   = 0;   /* total kfree() calls               */
static uint64_t heap_peak_bytes   = 0;
static bool heap_initialized = false;

static heap_block *heap_free_head = NULL;

static uint64_t align_up16(uint64_t v) {
    return (v + 15UL) & ~(uint64_t)15UL;
}

static uint64_t align_up_page(uint64_t v) {
    return (v + HEAP_PAGE_SIZE - 1U) & ~(uint64_t)(HEAP_PAGE_SIZE - 1U);
}

static uint64_t blk_size(uint64_t sf)    { return sf & ~1ULL; }
static bool     blk_is_used(uint64_t sf) { return (sf & 1ULL) != 0ULL; }

/* Free blocks store their list links inside their own payload. */
static heap_block **fl_prev(heap_block *b) {
    return (heap_block **)((uint8_t *)b + HEAP_HEADER_SIZE);
}
static heap_block **fl_next(heap_block *b) {
    return (heap_block **)((uint8_t *)b + HEAP_HEADER_SIZE + 8U);
}

static void fl_remove(heap_block *b) {
    heap_block *p = *fl_prev(b);
    heap_block *n = *fl_next(b);
    if (p != NULL) {
        *fl_next(p) = n;
    } else {
        heap_free_head = n;
    }
    if (n != NULL) {
        *fl_prev(n) = p;
    }
    *fl_prev(b) = NULL;
    *fl_next(b) = NULL;
}

static void fl_insert(heap_block *b) {
    *fl_prev(b) = NULL;
    *fl_next(b) = heap_free_head;
    if (heap_free_head != NULL) {
        *fl_prev(heap_free_head) = b;
    }
    heap_free_head = b;
}

static void blk_set_free(heap_block *b, uint64_t size) {
    b->size = size;
    b->magic = 0;
    *(uint64_t *)((uint8_t *)b + size - HEAP_FOOTER_SIZE) = size;
}

static void blk_set_used(heap_block *b, uint64_t size) {
    b->size = size | 1ULL;
    b->magic = HEAP_MAGIC;
    *(uint64_t *)((uint8_t *)b + size - HEAP_FOOTER_SIZE) = size | 1ULL;
}

static heap_block *block_next(heap_block *b) {
    return (heap_block *)((uint8_t *)b + blk_size(b->size));
}

/* The block that ends exactly at the end of the mapped region. */
static heap_block *heap_top(void) {
    if (heap_mapped_bytes == 0) {
        return NULL;
    }
    uint64_t end = HEAP_VIRT_BASE + heap_mapped_bytes;
    uint64_t last_size = blk_size(*(uint64_t *)(end - HEAP_FOOTER_SIZE));
    return (heap_block *)(end - last_size);
}
/* Map more physical pages and add them to the heap as free space. */
static bool heap_grow(uint64_t need_size) {
    uint64_t extra = align_up_page(need_size);
    if (heap_mapped_bytes + extra > HEAP_ARENA_SIZE) {
        return false;
    }
    uint64_t old_end = heap_mapped_bytes;
    uint64_t new_end = old_end;

    while (new_end - old_end < extra) {
        uint64_t phys = pmm_alloc_page();
        if (phys == 0) {
            break;   /* physical memory exhausted */
        }
        uint64_t virt = HEAP_VIRT_BASE + new_end;
        /* Zero the fresh page through the HHDM identity mapping. */
        memset((void *)(uintptr_t)vm_phys_to_virt(phys), 0, HEAP_PAGE_SIZE);
        if (!vm_map_page(virt, phys, VMM_PAGE_PRESENT | VMM_PAGE_WRITABLE)) {
            pmm_free_page(phys);
            break;
        }
        new_end += HEAP_PAGE_SIZE;
    }
    if (new_end == old_end) {
        return false;
    }

    /* Add the new pages to the heap. If the last block is free, just
     * grow it; otherwise create a fresh free block. */
    size_t add = (size_t)(new_end - old_end);
    heap_block *t = heap_top();
    if (t != NULL && !blk_is_used(t->size)) {
        uint64_t tsz = blk_size(t->size);
        blk_set_free(t, tsz + add);
        heap_free_total += add;
    } else {
        heap_block *nb = (heap_block *)(HEAP_VIRT_BASE + old_end);
        blk_set_free(nb, add);
        fl_insert(nb);
        heap_free_total += add;
    }
    heap_mapped_bytes = new_end;
    return true;
}

/* Find the first free block big enough, splitting it if there's room. */
static void *heap_alloc_from_free(uint64_t total) {
    for (heap_block *b = heap_free_head; b != NULL; b = *fl_next(b)) {
        uint64_t bsz = blk_size(b->size);
        if (bsz < total) {
            continue;
        }
        uint64_t rem = bsz - total;
        if (rem >= HEAP_MIN_BLOCK) {
            fl_remove(b);
            blk_set_used(b, total);
            heap_block *r = (heap_block *)((uint8_t *)b + total);
            blk_set_free(r, rem);
            fl_insert(r);
            heap_free_total -= total;
            heap_used_bytes += total - HEAP_HEADER_SIZE - HEAP_FOOTER_SIZE;
        } else {
            /* Remainder too small to be a real block: consume the whole block. */
            fl_remove(b);
            blk_set_used(b, bsz);
            heap_free_total -= bsz;
            heap_used_bytes += bsz - HEAP_HEADER_SIZE - HEAP_FOOTER_SIZE;
        }
        return (void *)((uint8_t *)b + HEAP_HEADER_SIZE);
    }
    return NULL;
}

/* Work out the full block size (header + payload + footer, aligned). */
static size_t block_total_for(size_t size) {
    size_t need = (size_t)align_up16(size) + HEAP_HEADER_SIZE + HEAP_FOOTER_SIZE;
    if (need < HEAP_MIN_BLOCK) {
        need = HEAP_MIN_BLOCK;
    }
    return (size_t)align_up16(need);
}

void *kmalloc(size_t size) {
    if (!heap_initialized) {
        kernel_panic("heap: kmalloc before heap_init");
    }
    if (size == 0 || size > HEAP_MAX_SIZE) {
        return NULL;
    }
    size_t total = block_total_for(size);
    void *p = heap_alloc_from_free(total);
    if (p == NULL && heap_grow(total)) {
        p = heap_alloc_from_free(total);
    }
    if (p != NULL) {
        ++heap_alloc_count;
        if (heap_used_bytes > heap_peak_bytes) {
            heap_peak_bytes = heap_used_bytes;
        }
    }
    return p;
}

void *kcalloc(size_t count, size_t size) {
    if (count != 0 && size > (size_t)-1 / count) {
        return NULL;
    }
    size_t total = count * size;
    void *p = kmalloc(total);
    if (p == NULL) {
        return NULL;
    }
    memset(p, 0, total);
    return p;
}

void *krealloc(void *ptr, size_t new_size) {
    if (ptr == NULL) {
        return kmalloc(new_size);
    }
    if (new_size == 0) {
        kfree(ptr);
        return NULL;
    }
    if (!heap_initialized) {
        kernel_panic("heap: krealloc before heap_init");
    }
    if (new_size > HEAP_MAX_SIZE) {
        return NULL;
    }
    heap_block *b = (heap_block *)((uint8_t *)ptr - HEAP_HEADER_SIZE);
    if (b->magic != HEAP_MAGIC || !blk_is_used(b->size)) {
        kernel_panic("heap: krealloc on invalid pointer");
    }
    size_t cap = (size_t)(blk_size(b->size) - HEAP_HEADER_SIZE - HEAP_FOOTER_SIZE);
    if (new_size <= cap) {
        return ptr;   /* no growth needed; do not shrink in place */
    }
    void *np = kmalloc(new_size);
    if (np == NULL) {
        return NULL;   /* out of memory: keep the old block intact */
    }
    memcpy(np, ptr, cap);
    kfree(ptr);
    return np;
}

void kfree(void *ptr) {
    if (!heap_initialized) {
        kernel_panic("heap: kfree before heap_init");
    }
    if (ptr == NULL) {
        return;
    }
    heap_block *b = (heap_block *)((uint8_t *)ptr - HEAP_HEADER_SIZE);
    if (b->magic != HEAP_MAGIC || !blk_is_used(b->size)) {
        kernel_panic("heap: kfree on corrupted or invalid pointer");
    }

    uint64_t bsz = blk_size(b->size);
    blk_set_free(b, bsz);
    heap_used_bytes -= bsz - HEAP_HEADER_SIZE - HEAP_FOOTER_SIZE;
    heap_free_total += bsz;
    --heap_alloc_count;
    ++heap_free_calls;

    uint64_t end = HEAP_VIRT_BASE + heap_mapped_bytes;

    /* Coalesce with the next block when it is also free. */
    heap_block *n = block_next(b);
    if ((uint64_t)(uintptr_t)n < end && !blk_is_used(n->size)) {
        fl_remove(n);
        uint64_t nsz = blk_size(n->size);
        bsz += nsz;
        blk_set_free(b, bsz);
    }

    /* Coalesce with the previous block, detected via its footer tag. */
    if ((uint64_t)(uintptr_t)b > HEAP_VIRT_BASE) {
        uint64_t prev_sf = *(uint64_t *)((uint8_t *)b - HEAP_FOOTER_SIZE);
        if (!blk_is_used(prev_sf)) {
            heap_block *p = (heap_block *)((uint8_t *)b - blk_size(prev_sf));
            fl_remove(p);
            uint64_t psz = blk_size(p->size);
            b = (heap_block *)((uint8_t *)p);
            blk_set_free(b, psz + bsz);
        }
    }

    fl_insert(b);
}
void heap_get_stats(heap_stats *out) {
    if (out == NULL) {
        return;
    }
    out->arena_bytes  = HEAP_ARENA_SIZE;
    out->mapped_bytes = heap_mapped_bytes;
    out->used_bytes   = heap_used_bytes;
    out->free_bytes   = heap_free_total;
    out->alloc_count  = heap_alloc_count;
    out->free_count   = heap_free_calls;
    out->peak_bytes   = heap_peak_bytes;
}

static size_t heap_capacity(const void *ptr) {
    const heap_block *b = (const heap_block *)((const uint8_t *)ptr - HEAP_HEADER_SIZE);
    return (size_t)(blk_size(b->size) - HEAP_HEADER_SIZE - HEAP_FOOTER_SIZE);
}

/* Walk every block and check the heap is still consistent. */
static void heap_validate(const char *where) {
    (void)where;
    uint64_t end = HEAP_VIRT_BASE + heap_mapped_bytes;
    uint64_t addr = HEAP_VIRT_BASE;
    uint64_t walk_used = 0, walk_free = 0;
    uint64_t walk_free_blocks = 0;

    while (addr < end) {
        heap_block *b = (heap_block *)addr;
        uint64_t sz = blk_size(b->size);
        if (sz < HEAP_MIN_BLOCK || (sz & 15UL) != 0) {
            kernel_panic("heap validate: bad block size");
        }
        if (addr + sz > end) {
            kernel_panic("heap validate: block overruns mapped region");
        }
        uint64_t foot = *(uint64_t *)(addr + sz - HEAP_FOOTER_SIZE);
        if (foot != b->size) {
            kernel_panic("heap validate: footer boundary-tag mismatch");
        }
        if (blk_is_used(b->size)) {
            if (b->magic != HEAP_MAGIC) {
                kernel_panic("heap validate: used block missing magic");
            }
            walk_used += sz - HEAP_HEADER_SIZE - HEAP_FOOTER_SIZE;
        } else {
            walk_free += sz;
            ++walk_free_blocks;
        }
        addr += sz;
    }
    if (addr != end) {
        kernel_panic("heap validate: blocks do not tile mapped region");
    }

    uint64_t cap = (end - HEAP_VIRT_BASE) / HEAP_MIN_BLOCK;
    uint64_t list_count = 0;
    for (heap_block *b = heap_free_head; b != NULL; b = *fl_next(b)) {
        if (blk_is_used(b->size)) {
            kernel_panic("heap validate: used block in free list");
        }
        if (++list_count > cap) {
            kernel_panic("heap validate: free list cycle");
        }
    }
    if (list_count != walk_free_blocks) {
        kernel_panic("heap validate: free-list node count mismatch");
    }
    if (walk_used != heap_used_bytes || walk_free != heap_free_total) {
        kernel_panic("heap validate: allocator accounting mismatch");
    }
}

void heap_init(void) {
    if (heap_initialized) {
        return;
    }
    if (!pmm_is_initialized()) {
        kernel_panic("heap_init requires PMM");
    }
    if (!vm_is_initialized()) {
        kernel_panic("heap_init requires VMM");
    }
    heap_free_head = NULL;
    heap_mapped_bytes = 0;
    heap_used_bytes = 0;
    heap_free_total = 0;
    heap_alloc_count = 0;
    heap_free_calls = 0;
    heap_peak_bytes = 0;
    if (!heap_grow(HEAP_INIT_SIZE)) {
        kernel_panic("heap_init: could not map initial region");
    }
    heap_initialized = true;
    heap_validate("heap_init");
}

bool heap_is_initialized(void) {
    return heap_initialized;
}
/* Fill an allocation with a known pattern so we can detect corruption. */
static void heap_fill_pattern(void *ptr, size_t bytes, uint8_t base) {
    uint8_t *p = (uint8_t *)ptr;
    for (size_t i = 0; i < bytes; ++i) {
        p[i] = (uint8_t)(base + i);
    }
}

static bool heap_verify_pattern(void *ptr, size_t bytes, uint8_t base) {
    uint8_t *p = (uint8_t *)ptr;
    size_t cap = heap_capacity(ptr);
    for (size_t i = 0; i < bytes && i < cap; ++i) {
        if (p[i] != (uint8_t)(base + i)) {
            return false;
        }
    }
    return true;
}

void heap_self_test(void) {
    if (!heap_initialized) {
        kernel_panic("heap_self_test requires heap_init");
    }
    heap_validate("selftest:start");

    /* kmalloc(0) must fail cleanly, not corrupt the heap. */
    if (kmalloc(0) != NULL) {
        kernel_panic("heap_self_test: kmalloc(0) returned non-NULL");
    }

    /* A handful of small allocations of distinct sizes. */
    void *a = kmalloc(1);
    void *b = kmalloc(24);
    void *c = kmalloc(40);
    if (a == NULL || b == NULL || c == NULL) {
        kernel_panic("heap_self_test: small allocation failed");
    }
    if (((uintptr_t)a & 15UL) || ((uintptr_t)b & 15UL) || ((uintptr_t)c & 15UL)) {
        kernel_panic("heap_self_test: small allocation not 16-aligned");
    }
    if (a == b || a == c || b == c) {
        kernel_panic("heap_self_test: overlapping small allocations");
    }

    /* Large allocation spanning pages. */
    void *big = kmalloc(2 * 1024 * 1024);
    if (big == NULL || ((uintptr_t)big & 15UL) != 0) {
        kernel_panic("heap_self_test: large allocation failed/misaligned");
    }

    /* Fill and verify each live allocation to catch overlap/corruption. */
    heap_fill_pattern(a, 1, 0x10);
    heap_fill_pattern(b, 24, 0x20);
    heap_fill_pattern(c, 40, 0x30);
    heap_fill_pattern(big, 2 * 1024 * 1024, 0x40);
    if (!heap_verify_pattern(a, 1, 0x10) ||
        !heap_verify_pattern(b, 24, 0x20) ||
        !heap_verify_pattern(c, 40, 0x30) ||
        !heap_verify_pattern(big, 2 * 1024 * 1024, 0x40)) {
        kernel_panic("heap_self_test: live-allocation data corrupted");
    }

    /* Free the small ones and confirm the heap stays aligned/valid. */
    kfree(a);
    kfree(b);
    kfree(c);
    heap_validate("selftest:after-free");

    /* Freed space is reused. */
    void *d = kmalloc(24);
    if (d == NULL) {
        kernel_panic("heap_self_test: could not reuse freed space");
    }
    kfree(d);

    /* kcalloc zero-init, plus its integer-overflow guard. */
    void *z = kcalloc(64, 8);
    if (z == NULL) {
        kernel_panic("heap_self_test: kcalloc returned NULL");
    }
    for (int i = 0; i < 64 * 8; ++i) {
        if (((uint8_t *)z)[i] != 0) {
            kernel_panic("heap_self_test: kcalloc not zero-initialized");
        }
    }
    kfree(z);
    if (kcalloc((size_t)-1, 8) != NULL) {
        kernel_panic("heap_self_test: kcalloc overflow guard failed");
    }

    /* krealloc: grow must preserve existing bytes; shrink must keep head. */
    void *r = kmalloc(16);
    if (r == NULL) {
        kernel_panic("heap_self_test: krealloc source failed");
    }
    heap_fill_pattern(r, 16, 0x70);
    void *g = krealloc(r, 4096);
    if (g == NULL) {
        kernel_panic("heap_self_test: krealloc(grow) returned NULL");
    }
    if (!heap_verify_pattern(g, 16, 0x70)) {
        kernel_panic("heap_self_test: krealloc lost existing data");
    }
    void *s = krealloc(g, 8);
    if (s == NULL || !heap_verify_pattern(s, 8, 0x70)) {
        kernel_panic("heap_self_test: krealloc(shrink) failed");
    }
    kfree(s);
    if (krealloc(NULL, 32) == NULL) {
        kernel_panic("heap_self_test: krealloc(NULL) failed");
    }

    heap_validate("selftest:after-krealloc");

    /* Deterministic pseudo-random allocation / free pattern. */
    enum { N_BLOCKS = 64 };
    void *slots[N_BLOCKS];
    size_t slots_size[N_BLOCKS];
    uint64_t lcg = 0x1111222233334444ULL;
    for (int i = 0; i < N_BLOCKS; ++i) {
        slots[i] = NULL;
        slots_size[i] = 0;
    }
    for (int round = 0; round < 6; ++round) {
        for (int i = 0; i < N_BLOCKS; ++i) {
            lcg = lcg * 6364136223846793005ULL + 1442695040888963407ULL;
            size_t want = (size_t)((lcg >> 33) % 400) + 8;
            if (slots[i] == NULL) {
                slots[i] = kmalloc(want);
                if (slots[i] == NULL) {
                    kernel_panic("heap_self_test: random alloc failed");
                }
                if (((uintptr_t)slots[i] & 15UL) != 0) {
                    kernel_panic("heap_self_test: random alloc misaligned");
                }
                slots_size[i] = want;
                heap_fill_pattern(slots[i], want, (uint8_t)i);
            } else {
                if (!heap_verify_pattern(slots[i], slots_size[i], (uint8_t)i)) {
                    kernel_panic("heap_self_test: random block corrupted");
                }
                kfree(slots[i]);
                slots[i] = NULL;
            }
        }
    }
    for (int i = 0; i < N_BLOCKS; ++i) {
        if (slots[i] != NULL) {
            kfree(slots[i]);
            slots[i] = NULL;
        }
    }

    heap_validate("selftest:after-random");

    /* Integer/overflow guard at the public API boundary. */
    if (kmalloc((size_t)-1) != NULL) {
        kernel_panic("heap_self_test: kmalloc(SIZE_MAX) not rejected");
    }

    /* Out-of-memory at the arena level: a request too large for the
     * 1GiB arena must return NULL without corrupting the heap. This avoids
     * exhausting physical memory (which would otherwise trip the VMM page
     * table allocator and turn the test into a bang). */
    if (kmalloc(HEAP_ARENA_SIZE + 65536) != NULL) {
        kernel_panic("heap_self_test: oversized arena request not rejected");
    }

    heap_validate("selftest:after-oom");

    /* Free the one big allocation; the heap must still be fully valid. */
    kfree(big);
    heap_validate("selftest:end");
}