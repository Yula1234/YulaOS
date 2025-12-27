#include <stdint.h>

#include "string.h"

size_t strlen(const char* s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
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

size_t strlcpy(char* dst, const char* src, size_t dstsz) {
    size_t i = 0;
    if (dstsz) {
        for (; i + 1 < dstsz && src[i]; i++) dst[i] = src[i];
        dst[i] = 0;
    }
    while (src[i]) i++;
    return i;
}

size_t strlcat(char* dst, const char* src, size_t dstsz) {
    size_t dlen = strlen(dst);
    size_t slen = strlen(src);

    if (dlen >= dstsz) return dstsz + slen;

    size_t to_copy = dstsz - dlen - 1;
    if (slen < to_copy) to_copy = slen;

    memcpy(dst + dlen, src, to_copy);
    dst[dlen + to_copy] = '\0';

    return dlen + slen;
}

__attribute__((target("sse2")))
void* memcpy_sse(void* dest, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dest;
    const uint8_t* s = (const uint8_t*)src;

    while (n >= 64) {
        __asm__ volatile (
            "movups (%0), %%xmm0\n"
            "movups 16(%0), %%xmm1\n"
            "movups 32(%0), %%xmm2\n"
            "movups 48(%0), %%xmm3\n"
            "movups %%xmm0, (%1)\n"
            "movups %%xmm1, 16(%1)\n"
            "movups %%xmm2, 32(%1)\n"
            "movups %%xmm3, 48(%1)\n"
            : : "r"(s), "r"(d) : "memory", "xmm0", "xmm1", "xmm2", "xmm3"
        );
        s += 64; d += 64; n -= 64;
    }

    while (n--) *d++ = *s++;
    return dest;
}

__attribute__((target("sse2")))
void* memset_sse(void* dest, int val, size_t n) {
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

void* memcpy(void* dst, const void* src, size_t n) {
    if (n < 64) {
        uint8_t* d = (uint8_t*)dst;
        const uint8_t* s = (const uint8_t*)src;
        while (n--) *d++ = *s++;
        return dst;
    }
    return memcpy_sse(dst, src, n);
}

void* memset(void* dst, int v, size_t n) {
    if (n < 64) {
        uint8_t* p = (uint8_t*)dst;
        while (n--) *p++ = (uint8_t)v;
        return dst;
    }
    return memset_sse(dst, v, n);
}