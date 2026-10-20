#include "lib/mem.h"

void *memcpy(void *dest, const void *src, size_t n) {
    volatile unsigned char *d = (volatile unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;
    for (size_t i = 0; i < n; ++i) {
        d[i] = s[i];
    }
    return dest;
}

void *memset(void *dest, int value, size_t n) {
    volatile unsigned char *d = (volatile unsigned char *)dest;
    unsigned char v = (unsigned char)value;
    for (size_t i = 0; i < n; ++i) {
        d[i] = v;
    }
    return dest;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    for (size_t i = 0; i < n; ++i) {
        if (x[i] != y[i]) {
            return (int)x[i] - (int)y[i];
        }
    }
    return 0;
}
