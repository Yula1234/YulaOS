// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#ifndef LIB_STRING_H
#define LIB_STRING_H

#include <stddef.h>
#include <stdint.h>

size_t strlen(const char* s);
int strcmp(const char* a, const char* b);
int strncmp(const char* a, const char* b, size_t n);

void* memset(void* dst, int v, size_t n);
void* memcpy(void* dst, const void* src, size_t n);

size_t strlcpy(char* dst, const char* src, size_t dstsz);
size_t strlcat(char* dst, const char* src, size_t dstsz);

void* memcpy_sse(void* dest, const void* src, size_t n);
void* memset_sse(void* dest, int val, size_t n);

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

#endif