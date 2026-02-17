// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#ifndef LIB_STRING_H
#define LIB_STRING_H

#include <stddef.h>
#include <stdint.h>

#ifndef restrict
#ifdef __cplusplus
#define restrict __restrict
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

__attribute__((target("sse2"))) size_t strlen(const char* s);
int strcmp(const char* a, const char* b);
int strncmp(const char* a, const char* b, size_t n);

__attribute__((target("sse2"))) void* memset(void* dst, int v, size_t n);
__attribute__((target("sse2"))) void* memcpy(void* restrict dst, const void* restrict src, size_t n);
__attribute__((target("sse2"))) void* memmove(void* dst, const void* src, size_t n);
int memcmp(const void* a, const void* b, size_t n);

size_t strlcpy(char* restrict dst, const char* restrict src, size_t dstsz);
size_t strlcat(char* restrict dst, const char* restrict src, size_t dstsz);

static inline int memcpy_safe(void* dst, size_t dst_size, const void* src, size_t src_size) {
    if (!dst || !src || dst_size == 0 || src_size == 0) return 0;
    if (src_size > dst_size) return 0;
    
    memcpy(dst, src, src_size);
    return 1;
}

static inline int strcpy_safe(char* dst, size_t dst_size, const char* src) {
    if (!dst || !src || dst_size == 0) return 0;
    size_t src_len = strlen(src);
    if (src_len >= dst_size) return 0;
    memcpy(dst, src, src_len + 1);
    return 1;
}

#ifdef __cplusplus
}
#endif

#endif
