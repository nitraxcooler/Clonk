#ifndef HEAP_H
#define HEAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HEAP_ALIGN         16UL
#define HEAP_BLOCK_ALIGN   16UL

#define HEAP_ARENA_SIZE    0x40000000ULL

#define HEAP_MAGIC         0x48454150514D4F4DULL

#define HEAP_HEADER_SIZE   16UL
#define HEAP_FOOTER_SIZE   8UL
#define HEAP_MIN_BLOCK     48UL

typedef struct heap_stats {
    uint64_t arena_bytes;
    uint64_t mapped_bytes;
    uint64_t used_bytes;
    uint64_t free_bytes;
    uint64_t alloc_count;
    uint64_t free_count;
    uint64_t peak_bytes;
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
