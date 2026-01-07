// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include <stdint.h>

#include "string.h"

    
#if defined(__GNUC__) || defined(__clang__)
    #define likely(x)       __builtin_expect(!!(x), 1)
    #define unlikely(x)     __builtin_expect(!!(x), 0)
#else
    #define likely(x)       (x)
    #define unlikely(x)     (x)
#endif



__attribute__((target("sse2")))
size_t strlen(const char* s) {
    const char* start = s;
    
    while (((uint32_t)s & 0xF) != 0) {
        if (*s == 0) return s - start;
        s++;
    }
    
    size_t res;
    
    __asm__ volatile (
        "pxor    %%xmm0, %%xmm0 \n\t"
        
        "1: \n\t"
        "movdqa  (%1), %%xmm1 \n\t"
        "pcmpeqb %%xmm0, %%xmm1 \n\t"
        "pmovmskb %%xmm1, %%eax \n\t"
        "test    %%eax, %%eax \n\t"
        "jnz     2f \n\t" 
        
        "add     $16, %1 \n\t"
        "jmp     1b \n\t"
        
        "2: \n\t"
        "bsf     %%eax, %%eax \n\t"  
        "add     %%eax, %1 \n\t" 
        "mov     %1, %0 \n\t"  
        
        : "=r"(res), "+r"(s)   
        :      
        : "eax", "xmm0", "xmm1", "cc" 
    );
    
    return res - (size_t)start;
}

int strcmp(const char* a, const char* b) {
    while (*a && (*a == *b)) { a++; b++; }
    return *(const unsigned char*)a - *(const unsigned char*)b;
}

int strncmp(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char ac = (unsigned char)a[i];
        unsigned char bc = (unsigned char)b[i];
        if (ac != bc) return ac - bc;
        if (!ac) return 0;
    }
    return 0;
}

size_t strlcpy(char* restrict dst, const char* restrict src, size_t dstsz) {
    size_t i = 0;
    if (dstsz) {
        for (; i + 1 < dstsz && src[i]; i++) dst[i] = src[i];
        dst[i] = 0;
    }
    while (src[i]) i++;
    return i;
}

size_t strlcat(char* restrict dst, const char* restrict src, size_t dstsz) {
    size_t dlen = strlen(dst);
    size_t slen = strlen(src);

    if (dlen >= dstsz) return dstsz + slen;

    size_t to_copy = dstsz - dlen - 1;
    if (slen < to_copy) to_copy = slen;

    memcpy(dst + dlen, src, to_copy);
    dst[dlen + to_copy] = '\0';

    return dlen + slen;
}

__attribute__((target("sse2"))) __attribute__((always_inline))
static inline void* memcpy_sse(void *restrict dest, const void *restrict src, size_t n) {
    uint8_t* d = (uint8_t*)dest;
    const uint8_t* s = (const uint8_t*)src;

    while (n >= 64) {
        __asm__ volatile (
            "movups (%0), %%xmm0 \n\t"
            "movups 16(%0), %%xmm1 \n\t"
            "movups 32(%0), %%xmm2 \n\t"
            "movups 48(%0), %%xmm3 \n\t"
            "movups %%xmm0, (%1) \n\t"
            "movups %%xmm1, 16(%1) \n\t"
            "movups %%xmm2, 32(%1) \n\t"
            "movups %%xmm3, 48(%1) \n\t"
            : : "r"(s), "r"(d) 
            : "memory", "xmm0", "xmm1", "xmm2", "xmm3"
        );
        s += 64; d += 64; n -= 64;
    }
    
    return d; 
}

__attribute__((target("sse2"))) __attribute__((always_inline))
static inline void* memset_sse(void* dest, int val, size_t n) {
    uint8_t* d = (uint8_t*)dest;
    uint32_t v = (uint8_t)val;
    v |= (v << 8) | (v << 16) | (v << 24);

    __asm__ volatile (
        "movd %0, %%xmm0\n"
        "pshufd $0, %%xmm0, %%xmm0\n"
        : : "r"(v) : "xmm0"
    );

    while (n >= 64) {
        __asm__ volatile (
            "movups %%xmm0, (%0)\n"
            "movups %%xmm0, 16(%0)\n"
            "movups %%xmm0, 32(%0)\n"
            "movups %%xmm0, 48(%0)\n"
            : : "r"(d) : "memory"
        );
        d += 64; n -= 64;
    }

    while (n--) *d++ = (uint8_t)val;
    return dest;
}

__attribute__((target("sse2")))
void* memcpy(void *restrict dst, const void *restrict src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;

    if (unlikely(n >= 64)) {
        (void)memcpy_sse(d, s, n);

        s += (n - (n % 64));
        d += (n - (n % 64));
        n %= 64;
    }

    while (n >= 4) {
        *(uint32_t *)d = *(const uint32_t *)s;
        d += 4;
        s += 4;
        n -= 4;
    }

    while (n--) {
        *d++ = *s++;
    }

    return dst;
}

__attribute__((target("sse2")))
void* memset(void* dst, int v, size_t n) {
    if (n < 64) {
        uint8_t* p = (uint8_t*)dst;
        while (n--) *p++ = (uint8_t)v;
        return dst;
    }
    return memset_sse(dst, v, n);
}