/* string.c - see string.h */
#include "string.h"

void *memset(void *dest, int value, size_t count) {
    unsigned char *d = (unsigned char *)dest;
    unsigned char v = (unsigned char)value;
    for (size_t i = 0; i < count; i++) {
        d[i] = v;
    }
    return dest;
}

void *memcpy(void *dest, const void *src, size_t count) {
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;
    for (size_t i = 0; i < count; i++) {
        d[i] = s[i];
    }
    return dest;
}
