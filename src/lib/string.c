/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2025 Yula1234 */

#include <lib/compiler.h>

#include <mm/heap.h>

#include <stdint.h>

#include "string.h"

#if defined(__GNUC__) || defined(__clang__)
typedef uint32_t u32_alias __attribute__((__may_alias__));
#else
typedef uint32_t u32_alias;
#endif

int strcmp(const char* a, const char* b)
{
    while (1)
    {
        unsigned char ac = (unsigned char)*a;
        unsigned char bc = (unsigned char)*b;

        if (ac != bc)
        {
            return (int)ac - (int)bc;
        }

        if (ac == 0)
        {
            return 0;
        }

        a++;
        b++;
    }
}

size_t strlen(const char* s)
{
    const char* start = s;

    while (*s)
    {
        s++;
    }

    return (size_t)(s - start);
}

int strncmp(const char* a, const char* b, size_t n)
{
    if (n == 0)
    {
        return 0;
    }

    while (n > 0)
    {
        unsigned char ac = (unsigned char)*a;
        unsigned char bc = (unsigned char)*b;

        if (ac != bc)
        {
            return (int)ac - (int)bc;
        }

        if (ac == 0)
        {
            return 0;
        }

        a++;
        b++;
        n--;
    }

    return 0;
}

char* strdup(const char* s) {
    if (unlikely(!s)) {
        return 0;
    }

    const size_t len = strlen(s);

    char* out = (char*)kmalloc(len + 1u);
    if (unlikely(!out)) {
        return 0;
    }

    memcpy(out, s, len);
    out[len] = '\0';

    return out;
}

static size_t strnlen_impl(const char* s, size_t max)
{
    const char* p = s;

    while (max > 0 && *p)
    {
        p++;
        max--;
    }

    return (size_t)(p - s);
}

size_t strlcpy(char* restrict dst, const char* restrict src, size_t dstsz)
{
    size_t slen = strlen(src);

    if (dstsz == 0)
    {
        return slen;
    }

    size_t to_copy = slen;

    if (to_copy >= dstsz)
    {
        to_copy = dstsz - 1;
    }

    if (to_copy > 0)
    {
        memcpy(dst, src, to_copy);
    }

    dst[to_copy] = 0;

    return slen;
}

size_t strlcat(char* restrict dst, const char* restrict src, size_t dstsz)
{
    if (dstsz == 0)
    {
        return strlen(src);
    }

    size_t dlen = strnlen_impl(dst, dstsz);
    size_t slen = strlen(src);

    if (dlen == dstsz)
    {
        return dstsz + slen;
    }

    size_t to_copy = dstsz - dlen - 1;

    if (to_copy > 0)
    {
        if (slen < to_copy)
        {
            to_copy = slen;
        }

        if (to_copy > 0)
        {
            memcpy(dst + dlen, src, to_copy);
        }

        dst[dlen + to_copy] = '\0';
    }

    return dlen + slen;
}

void* memcpy(void* restrict dst, const void* restrict src, size_t n) {
    uint32_t d0, d1, d2;
    __asm__ volatile(
        "cld\n\t"
        "rep movsl\n\t"
        "mov %4, %%ecx\n\t"
        "and $3, %%ecx\n\t"
        "jz 1f\n\t"
        "rep movsb\n\t"
        "1:"
        : "=&c"(d0), "=&D"(d1), "=&S"(d2)
        : "0"(n >> 2), "r"(n), "1"(dst), "2"(src)
        : "memory", "cc"
    );
    return dst;
}


void* memmove(void* dst, const void* src, size_t n) {
    if (unlikely(n == 0 || dst == src)) {
        return dst;
    }
    
    uintptr_t d_ptr = (uintptr_t)dst;
    uintptr_t s_ptr = (uintptr_t)src;

    if (d_ptr < s_ptr || d_ptr >= s_ptr + n) {
        return memcpy(dst, src, n);
    }

    uint8_t* d = (uint8_t*)dst + n;

    const uint8_t* s = (const uint8_t*)src + n;

    while (n & 3) {
        *(--d) = *(--s);
        n--;
    }

    uint32_t* d32 = (uint32_t*)d;

    const uint32_t* s32 = (const uint32_t*)s;

    size_t dwords = n >> 2;

    while (dwords--) {
        *(--d32) = *(--s32);
    }

    return dst;
}


void* memset(void* dst, int v, size_t n) {
    uint32_t c32 = (uint8_t)v;
    
    c32 |= c32 << 8;
    c32 |= c32 << 16;
    
    uint32_t d0, d1;

    __asm__ volatile(
        "cld\n\t"
        "rep stosl\n\t"
        "mov %3, %%ecx\n\t"
        "and $3, %%ecx\n\t"
        "jz 1f\n\t"
        "rep stosb\n\t"
        "1:"
        : "=&c"(d0), "=&D"(d1)
        : "a"(c32), "r"(n), "0"(n >> 2), "1"(dst)
        : "memory", "cc"
    );
    return dst;
}


int memcmp(const void* a, const void* b, size_t n) {
    const uint8_t* pa = (const uint8_t*)a;
    const uint8_t* pb = (const uint8_t*)b;

    size_t dwords = n >> 2;

    const uint32_t* pa32 = (const uint32_t*)pa;
    const uint32_t* pb32 = (const uint32_t*)pb;

    while (dwords--) {
        if (*pa32 != *pb32) {
            pa = (const uint8_t*)pa32;
            pb = (const uint8_t*)pb32;

            for (int i = 0; i < 4; i++) {
                if (pa[i] != pb[i]) {
                    return (int)pa[i] - (int)pb[i];
                }
            }
        }

        pa32++;
        pb32++;
    }

    pa = (const uint8_t*)pa32;
    pb = (const uint8_t*)pb32;

    size_t bytes = n & 3;

    while (bytes--) {
        if (*pa != *pb) {
            return (int)*pa - (int)*pb;
        }

        pa++;
        pb++;
    }

    return 0;
}