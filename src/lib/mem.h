#ifndef MEM_H
#define MEM_H

#include <stddef.h>

/*
 * Tiny memory helpers for when there's no libc around.
 *
 * The compiler will quietly emit calls to memcpy/memset for things like
 * struct copies, so we have to provide them ourselves. They're just
 * plain byte loops — good enough, and the `volatile` keeps the compiler
 * from turning our loop back into a call to itself (yes, that's a thing).
 */

void *memcpy(void *dest, const void *src, size_t n);
void *memset(void *dest, int value, size_t n);
int   memcmp(const void *a, const void *b, size_t n);

#endif