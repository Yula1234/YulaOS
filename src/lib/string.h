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

size_t strlen(const char* s);
int strcmp(const char* a, const char* b);
int strncmp(const char* a, const char* b, size_t n);

void* memset(void* dst, int v, size_t n);
void* memcpy(void* restrict dst, const void* restrict src, size_t n);
void* memmove(void* dst, const void* src, size_t n);
int memcmp(const void* a, const void* b, size_t n);

size_t strlcpy(char* restrict dst, const char* restrict src, size_t dstsz);
size_t strlcat(char* restrict dst, const char* restrict src, size_t dstsz);

#ifdef __cplusplus
}
#endif

#endif
