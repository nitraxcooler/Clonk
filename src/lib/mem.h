#ifndef MEM_H
#define MEM_H

#include <stddef.h>

/*
 * Minimal freestanding memory helpers.
 *
 * MyOS runs without a C runtime library, but the x86-64 ABI and compiler
 * codegen may emit calls to memcpy/memset for bulk copies and zeroing.
 * These provide those symbols so the kernel stays fully self-contained.
 *
 * They are intentionally simple (byte loops). The 'volatile' accessors stop
 * the compiler from recognising the loop as a call to the very function being
 * defined, which would otherwise recurse.
 */

void *memcpy(void *dest, const void *src, size_t n);
void *memset(void *dest, int value, size_t n);
int   memcmp(const void *a, const void *b, size_t n);

#endif