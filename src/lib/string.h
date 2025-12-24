#ifndef LIB_STRING_H
#define LIB_STRING_H

#include <stddef.h>

size_t strlen(const char* s);
int strcmp(const char* a, const char* b);
int strncmp(const char* a, const char* b, size_t n);

void* memset(void* dst, int v, size_t n);
void* memcpy(void* dst, const void* src, size_t n);

size_t strlcpy(char* dst, const char* src, size_t dstsz);
size_t strlcat(char* dst, const char* src, size_t dstsz);

void* memcpy_sse(void* dest, const void* src, size_t n);
void* memset_sse(void* dest, int val, size_t n);

#endif