/* string.h
 * Freestanding builds get none of libc's string functions for free -
 * only what's declared and implemented here.
 */
#ifndef STRING_H
#define STRING_H

#include <stddef.h>

void *memset(void *dest, int value, size_t count);
void *memcpy(void *dest, const void *src, size_t count);
int memcmp(const void *a, const void *b, size_t count);

#endif
