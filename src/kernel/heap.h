#ifndef HEAP_H
#define HEAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * MyOS kernel heap.
 *
 * A small boundary-tag (header/footer) free-list allocator built on top of
 * the existing Physical Memory Manager (PMM) and the Virtual Memory Manager
 * (VMM). The heap carves a reserved region of high-half virtual address
 * space and grows it one physical page at a time (scattered physical pages
 * are fine: VMM maps an arbitrary physical page to a chosen virtual page).
 *
 * Allocations are 16-byte aligned. The total block size is a multiple of 16
 * so that every block starts and ends on a 16-byte boundary.
 */

#define HEAP_ALIGN         16UL
#define HEAP_BLOCK_ALIGN   16UL

/* 0x48454150514D4F4D = "HEAPQMOM" stored in the header of used blocks. */
#define HEAP_MAGIC         0x48454150514D4F4DULL

/* Header: size|used (8) + magic (8).  Footer: size|used boundary tag (8). */
#define HEAP_HEADER_SIZE   16UL
#define HEAP_FOOTER_SIZE   8UL
#define HEAP_MIN_BLOCK     48UL   /* align16(header + links + footer) */

typedef struct heap_stats {
    uint64_t arena_bytes;    /* total reserved virtual arena              */
    uint64_t mapped_bytes;   /* bytes currently backed by physical pages  */
    uint64_t used_bytes;     /* payload bytes held by live allocations    */
    uint64_t free_bytes;     /* block bytes in the free list              */
    uint64_t alloc_count;    /* outstanding allocations                   */
    uint64_t free_count;     /* total kfree() calls served                */
    uint64_t peak_bytes;     /* all-time high-water mark of used_bytes    */
} heap_stats;

void heap_init(void);
bool heap_is_initialized(void);

void *kmalloc(size_t size);
void *kcalloc(size_t count, size_t size);
void *krealloc(void *ptr, size_t new_size);
void  kfree(void *ptr);

void heap_get_stats(heap_stats *out);
void heap_self_test(void);

#endif