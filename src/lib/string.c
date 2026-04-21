/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2025 Yula1234 */

#include <lib/compiler.h>

#include <mm/heap.h>

#include <stdint.h>

#include "string.h"

#include <stdint.h>

int strcmp(const char* a, const char* b) {
    if (unlikely(a == b)) {
        return 0;
    }

    while (((uintptr_t)a & 3) != 0) {
        unsigned char ac = (unsigned char)*a++;
        unsigned char bc = (unsigned char)*b++;

        if (ac != bc) {
            return (int)ac - (int)bc;
        }
        
        if (unlikely(!ac)) {
            return 0;
        }
    }

    while (1) {
        if (unlikely(((uintptr_t)b & 0xFFF) > 0xFFC)) {
            break; 
        }

        uint32_t va = *(const uint32_t*)a;
        uint32_t vb = *(const uint32_t*)b;

        uint32_t has_zero = (va - 0x01010101UL) & ~va & 0x80808080UL;

        if (unlikely(va != vb || has_zero)) {
            break;
        }

        a += 4;
        b += 4;
    }

    while (1) {
        unsigned char ac, bc;

        ac = (unsigned char)*a++;
        bc = (unsigned char)*b++;
        
        if (ac != bc || !ac) {
            return (int)ac - (int)bc;
        }

        ac = (unsigned char)*a++;
        bc = (unsigned char)*b++;
        
        if (ac != bc || !ac) {
            return (int)ac - (int)bc;
        }

        ac = (unsigned char)*a++;
        bc = (unsigned char)*b++;

        if (ac != bc || !ac) {
            return (int)ac - (int)bc;
        }

        ac = (unsigned char)*a++;
        bc = (unsigned char)*b++;
        
        if (ac != bc || !ac) {
            return (int)ac - (int)bc;
        }
    }
}

size_t strlen(const char* s) {
    const char* start = s;

    while (unlikely((uintptr_t)s & 3)) {
        if (!*s) {
            return s - start;
        }
        
        s++;
    }

    const uint32_t* p = (const uint32_t*)s;

    while (1) {
        uint32_t v0 = p[0];
        uint32_t z0 = (v0 - 0x01010101UL) & ~v0 & 0x80808080UL;

        if (unlikely(z0)) {
            s = (const char*)p;

            break;
        }

        uint32_t v1 = p[1];
        uint32_t z1 = (v1 - 0x01010101UL) & ~v1 & 0x80808080UL;

        if (unlikely(z1)) {
            s = (const char*)(p + 1);

            break;
        }

        uint32_t v2 = p[2];
        uint32_t z2 = (v2 - 0x01010101UL) & ~v2 & 0x80808080UL;

        if (unlikely(z2)) {
            s = (const char*)(p + 2);

            break;
        }

        uint32_t v3 = p[3];
        uint32_t z3 = (v3 - 0x01010101UL) & ~v3 & 0x80808080UL;

        if (unlikely(z3)) {
            s = (const char*)(p + 3);

            break;
        }

        p += 4;
    }

    uint32_t v = *(const uint32_t*)s;
    if ((v & 0x000000FF) == 0) {
        return s - start;
    }

    if ((v & 0x0000FF00) == 0) {
        return s - start + 1;
    }

    if ((v & 0x00FF0000) == 0) {
        return s - start + 2;
    }

    return s - start + 3;
}

int strncmp(const char* a, const char* b, size_t n) {
    if (n == 0) {
        return 0;
    }

    while (n > 0) {
        unsigned char ac = (unsigned char)*a;
        unsigned char bc = (unsigned char)*b;

        if (ac != bc) {
            return (int)ac - (int)bc;
        }

        if (ac == 0) {
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

size_t strlcpy(char* restrict dst, const char* restrict src, size_t dstsz) {
    const char* s = src;

    if (likely(dstsz > 0)) {
        char* d = dst;

        size_t n = dstsz - 1;

        while (n >= 4) {
            char c0 = s[0];

            if (!c0) {
                d[0] = '\0';

                return (size_t)(s - src);
            }
            
            d[0] = c0;

            char c1 = s[1];

            if (!c1) {
                d[1] = '\0';

                return (size_t)(s - src) + 1;
            }
            
            d[1] = c1;

            char c2 = s[2];

            if (!c2) {
                d[2] = '\0';

                return (size_t)(s - src) + 2;
            }
            
            d[2] = c2;

            char c3 = s[3];

            if (!c3) {
                d[3] = '\0';
            
                return (size_t)(s - src) + 3;
            }
            
            d[3] = c3;
            
            d += 4;
            s += 4;
            n -= 4;
        }

        while (n > 0) {
            char c = *s;

            if (!c) {
                *d = '\0';
            
                return (size_t)(s - src);
            
            }
            
            *d++ = c;
            
            s++;
            n--;
        }

        *d = '\0';
    }

    while ((uintptr_t)s & 3) {
        if (*s == '\0') {
            return (size_t)(s - src);
        }

        s++;
    }

    while (1) {
        uint32_t v = *(const uint32_t*)s;
        
        if (unlikely((v - 0x01010101UL) & ~v & 0x80808080UL)) {
            if ((v & 0xFF) == 0) {
                return (size_t)(s - src);
            }

            if ((v & 0xFF00) == 0) {
                return (size_t)(s - src) + 1;
            }

            if ((v & 0xFF0000) == 0) {
                return (size_t)(s - src) + 2;
            }

            return (size_t)(s - src) + 3;
        }

        s += 4;
    }
}

size_t strlcat(char* restrict dst, const char* restrict src, size_t dstsz) {
    char* d = dst;

    size_t n = dstsz;

    while (n >= 4) {
        uint32_t v = *(const uint32_t*)d;
        
        if (unlikely((v - 0x01010101UL) & ~v & 0x80808080UL)) {
            if ((v & 0xFF) == 0) {
                break;
            }

            if ((v & 0xFF00) == 0) {
                d += 1;
                n -= 1;
            
                break;
            }

            if ((v & 0xFF0000) == 0) {
                d += 2;
                n -= 2;
            
                break;
            }
            
            d += 3;
            n -= 3;
            
            break;
        }

        d += 4;
        n -= 4;
    }

    while (n > 0 && *d != '\0') {
        d++;
        n--;
    }

    size_t dlen = d - dst;

    if (unlikely(n == 0)) {
        const char* s = src;
        while (1) {
            if (s[0] == '\0') {
                break;
            }

            if (s[1] == '\0') {
                s += 1; break;
            }

            if (s[2] == '\0') {
                s += 2; break;
            }

            if (s[3] == '\0') {
                s += 3; break;
            }

            s += 4;
        }
        return dstsz + (s - src);
    }

    const char* s = src;
    size_t remaining = n - 1;

    while (remaining >= 4) {
        char c0 = s[0];
        if (!c0) {
            *d = '\0';

            return dlen + (s - src);
        }
        
        d[0] = c0;

        char c1 = s[1];
        if (!c1) {
            d[1] = '\0';

            return dlen + (s - src) + 1;
        }
        
        d[1] = c1;

        char c2 = s[2];
        if (!c2) {
            d[2] = '\0';

            return dlen + (s - src) + 2;
        }
        
        d[2] = c2;

        char c3 = s[3];
        if (!c3) {
            d[3] = '\0';

            return dlen + (s - src) + 3;
        }
        
        d[3] = c3;

        d += 4;
        s += 4;

        remaining -= 4;
    }

    while (remaining > 0) {
        char c = *s++;
        
        if (!c) {
            *d = '\0';

            return dlen + (s - src - 1);
        }
        
        *d++ = c;

        remaining--;
    }

    *d = '\0';

    while (1) {
        if (s[0] == '\0') {
            break;
        }
        
        if (s[1] == '\0') {
            s += 1; break;
        }
        
        if (s[2] == '\0') {
            s += 2; break;
        }

        if (s[3] == '\0') {
            s += 3; break;
        }
        
        s += 4;
    }

    return dlen + (s - src);
}

void* memcpy(void* restrict dst, const void* restrict src, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;

    if (unlikely(n < 4)) {
        if (unlikely(n == 0)) {
            return dst;
        }

        d[0] = s[0];
        
        if (n >= 2) {
            d[1] = s[1];
            d[n - 1] = s[n - 1];
        }

        return dst;
    }
    
    if (unlikely(n <= 8)) {
        uint32_t v0 = *(const uint32_t*)s;
        uint32_t v1 = *(const uint32_t*)(s + n - 4);
        
        *(uint32_t*)d = v0;
        *(uint32_t*)(d + n - 4) = v1;
        return dst;
    }
    
    if (unlikely(n <= 16)) {
        uint32_t v0 = *(const uint32_t*)s;
        uint32_t v1 = *(const uint32_t*)(s + 4);
        uint32_t v2 = *(const uint32_t*)(s + n - 8);
        uint32_t v3 = *(const uint32_t*)(s + n - 4);
        
        *(uint32_t*)d = v0;
        *(uint32_t*)(d + 4) = v1;
        *(uint32_t*)(d + n - 8) = v2;
        *(uint32_t*)(d + n - 4) = v3;
        return dst;
    }
    
    if (unlikely(n <= 32)) {
        uint32_t v0 = *(const uint32_t*)s;
        uint32_t v1 = *(const uint32_t*)(s + 4);
        uint32_t v2 = *(const uint32_t*)(s + 8);
        uint32_t v3 = *(const uint32_t*)(s + 12);
        
        uint32_t v4 = *(const uint32_t*)(s + n - 16);
        uint32_t v5 = *(const uint32_t*)(s + n - 12);
        uint32_t v6 = *(const uint32_t*)(s + n - 8);
        uint32_t v7 = *(const uint32_t*)(s + n - 4);

        *(uint32_t*)d = v0;
        *(uint32_t*)(d + 4) = v1;
        *(uint32_t*)(d + 8) = v2;
        *(uint32_t*)(d + 12) = v3;
        
        *(uint32_t*)(d + n - 16) = v4;
        *(uint32_t*)(d + n - 12) = v5;
        *(uint32_t*)(d + n - 8) = v6;
        *(uint32_t*)(d + n - 4) = v7;
        return dst;
    }

    uint32_t tail = *(const uint32_t*)(s + n - 4);

    size_t offset = (4 - ((uintptr_t)d & 3)) & 3;
    
    *(uint32_t*)d = *(const uint32_t*)s;

    uint32_t* d32 = (uint32_t*)(d + offset);
    const uint32_t* s32 = (const uint32_t*)(s + offset);
    
    const size_t dwords = (n - offset) >> 2;

    uint32_t dummy0, dummy1, dummy2;
    __asm__ volatile(
        "rep movsl\n\t" 
        : "=&c"(dummy0), "=&D"(dummy1), "=&S"(dummy2)
        : "0"(dwords), "1"(d32), "2"(s32)
        : "memory", "cc"
    );

    *(uint32_t*)(d + n - 4) = tail;

    return dst;
}


void* memmove(void* dst, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;

    if (unlikely(n == 0 || d == s)) {
        return dst;
    }

    if ((uintptr_t)d < (uintptr_t)s || (uintptr_t)d >= (uintptr_t)s + n) {
        return memcpy(dst, src, n);
    }

    if (unlikely(n < 4)) {
        uint8_t v0 = s[0];
        uint8_t v1 = s[n / 2];
        uint8_t v2 = s[n - 1];
        
        d[n - 1] = v2;
        d[n / 2] = v1;
        d[0] = v0;
        return dst;
    }
    
    if (unlikely(n <= 8)) {
        uint32_t v0 = *(const uint32_t*)s;
        uint32_t v1 = *(const uint32_t*)(s + n - 4);
        
        *(uint32_t*)(d + n - 4) = v1;
        *(uint32_t*)d = v0;
        return dst;
    }
    
    if (unlikely(n <= 16)) {
        uint32_t v0 = *(const uint32_t*)s;
        uint32_t v1 = *(const uint32_t*)(s + 4);
        uint32_t v2 = *(const uint32_t*)(s + n - 8);
        uint32_t v3 = *(const uint32_t*)(s + n - 4);
        
        *(uint32_t*)(d + n - 4) = v3;
        *(uint32_t*)(d + n - 8) = v2;
        *(uint32_t*)(d + 4) = v1;
        *(uint32_t*)d = v0;
        return dst;
    }

    uint32_t f0 = *(const uint32_t*)s;
    uint32_t f1 = *(const uint32_t*)(s + 4);
    uint32_t f2 = *(const uint32_t*)(s + 8);
    uint32_t f3 = *(const uint32_t*)(s + 12);

    size_t i = n;
    
    while (i >= 16) {
        i -= 16;
        uint32_t t0 = *(const uint32_t*)(s + i);
        uint32_t t1 = *(const uint32_t*)(s + i + 4);
        uint32_t t2 = *(const uint32_t*)(s + i + 8);
        uint32_t t3 = *(const uint32_t*)(s + i + 12);
        
        *(uint32_t*)(d + i + 12) = t3;
        *(uint32_t*)(d + i + 8)  = t2;
        *(uint32_t*)(d + i + 4)  = t1;
        *(uint32_t*)(d + i)      = t0;
    }

    *(uint32_t*)d = f0;
    *(uint32_t*)(d + 4) = f1;
    *(uint32_t*)(d + 8) = f2;
    *(uint32_t*)(d + 12) = f3;

    return dst;
}


void* memset(void* dst, int v, size_t n) {
    uint8_t* p = (uint8_t*)dst;

    const uint32_t c32 = (uint8_t)v * 0x01010101U;

    if (unlikely(n < 4)) {
        if (unlikely(n == 0)) {
            return dst;
        }
        
        p[0] = (uint8_t)v;
        
        if (n >= 2) {
            p[1] = (uint8_t)v;
            p[n - 1] = (uint8_t)v;
        }
        
        return dst;
    }

    if (unlikely(n <= 8)) {
        *(uint32_t*)p = c32;
        *(uint32_t*)(p + n - 4) = c32;

        return dst;
    }

    if (unlikely(n <= 16)) {
        *(uint32_t*)p = c32;
        *(uint32_t*)(p + 4) = c32;
        *(uint32_t*)(p + n - 8) = c32;
        *(uint32_t*)(p + n - 4) = c32;

        return dst;
    }

    if (unlikely(n <= 32)) {
        *(uint32_t*)p = c32;
        *(uint32_t*)(p + 4) = c32;
        *(uint32_t*)(p + 8) = c32;
        *(uint32_t*)(p + 12) = c32;
        *(uint32_t*)(p + n - 16) = c32;
        *(uint32_t*)(p + n - 12) = c32;
        *(uint32_t*)(p + n - 8) = c32;
        *(uint32_t*)(p + n - 4) = c32;

        return dst;
    }

    *(uint32_t*)p = c32;
    
    *(uint32_t*)(p + n - 4) = c32;

    const size_t offset = (4 - ((uintptr_t)p & 3)) & 3;
    uint32_t* p_aligned = (uint32_t*)(p + offset);
    
    const size_t dwords = (n - offset) >> 2;

    uint32_t d0, d1;
    
    __asm__ volatile (
        "rep stosl\n\t"
        : "=&c"(d0), "=&D"(d1)
        : "a"(c32), "0"(dwords), "1"(p_aligned)
        : "memory", "cc"
    );

    return dst;
}

int memcmp(const void* a, const void* b, size_t n) {
    const uint8_t* pa = (const uint8_t*)a;
    const uint8_t* pb = (const uint8_t*)b;

    while (n >= 4) {
        uint32_t va = *(const uint32_t*)pa;
        uint32_t vb = *(const uint32_t*)pb;

        if (likely(va != vb)) {
            uint32_t diff = va ^ vb;
            
            uint32_t shift = __builtin_ctz(diff) & ~7u;
            
            return (int)((va >> shift) & 0xFF) - (int)((vb >> shift) & 0xFF);
        }
        
        pa += 4; pb += 4;
        n -= 4;
    }

    if (n >= 2) {
        uint16_t va = *(const uint16_t*)pa;
        uint16_t vb = *(const uint16_t*)pb;
        
        if (likely(va != vb)) {
            uint32_t diff = va ^ vb;

            uint32_t shift = __builtin_ctz(diff) & ~7u;
            
            return (int)((va >> shift) & 0xFF) - (int)((vb >> shift) & 0xFF);
        }

        pa += 2; pb += 2;
        n -= 2;
    }

    if (unlikely(n > 0)) {
        return (int)*pa - (int)*pb;
    }

    return 0;
}

void memzero_nt_page(void* dst) {
    uint32_t count = 64; 
    const uint32_t zero = 0;

    __asm__ volatile (
        ".align 16\n"
        "1:\n\t"
        "movnti %[z],   0(%[p])\n\t"
        "movnti %[z],   4(%[p])\n\t"
        "movnti %[z],   8(%[p])\n\t"
        "movnti %[z],  12(%[p])\n\t"
        "movnti %[z],  16(%[p])\n\t"
        "movnti %[z],  20(%[p])\n\t"
        "movnti %[z],  24(%[p])\n\t"
        "movnti %[z],  28(%[p])\n\t"
        "movnti %[z],  32(%[p])\n\t"
        "movnti %[z],  36(%[p])\n\t"
        "movnti %[z],  40(%[p])\n\t"
        "movnti %[z],  44(%[p])\n\t"
        "movnti %[z],  48(%[p])\n\t"
        "movnti %[z],  52(%[p])\n\t"
        "movnti %[z],  56(%[p])\n\t"
        "movnti %[z],  60(%[p])\n\t"
        "add $64, %[p]\n\t"
        "dec %[c]\n\t"
        "jnz 1b\n\t"
        "sfence\n\t"
        : [p] "+r" (dst), [c] "+r" (count)
        : [z] "r" (zero)
        : "memory", "cc"
    );
}