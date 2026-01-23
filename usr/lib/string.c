// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include "string.h"

extern void* malloc(size_t size);

#if defined(__GNUC__) || defined(__clang__)
 #define likely(x)   __builtin_expect(!!(x), 1)
 #define unlikely(x) __builtin_expect(!!(x), 0)
#else
 #define likely(x)   (x)
 #define unlikely(x) (x)
#endif

#if defined(__GNUC__) || defined(__clang__)
typedef uint32_t u32_alias __attribute__((__may_alias__));
#else
typedef uint32_t u32_alias;
#endif

#define PAGE_SIZE 4096u
#define PAGE_MASK (PAGE_SIZE - 1u)

__attribute__((always_inline))
static inline uintptr_t page_off(const void* p) {
    return (uintptr_t)p & (uintptr_t)PAGE_MASK;
}

__attribute__((always_inline))
static inline int cpu_has_cpuid(void) {
    uint32_t a, b;
    __asm__ volatile(
        "pushfl\n\t"
        "popl %0\n\t"
        : "=r"(a)
        :
        : "cc", "memory"
    );
    b = a ^ (1u << 21);
    __asm__ volatile(
        "pushl %0\n\t"
        "popfl\n\t"
        :
        : "r"(b)
        : "cc", "memory"
    );
    __asm__ volatile(
        "pushfl\n\t"
        "popl %0\n\t"
        : "=r"(b)
        :
        : "cc", "memory"
    );
    return ((a ^ b) & (1u << 21)) != 0u;
}

__attribute__((always_inline))
static inline int cpu_has_sse2(void) {
    static int cached = -1;
    if (cached != -1) return cached;
    if (!cpu_has_cpuid()) {
        cached = 0;
        return cached;
    }
    uint32_t eax, ebx, ecx, edx;
    eax = 1;
    __asm__ volatile(
        "cpuid"
        : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        :
        : "cc", "memory"
    );
    cached = ((edx & (1u << 26)) != 0u) ? 1 : 0;
    return cached;
}

__attribute__((target("sse2"))) __attribute__((always_inline))
static inline uint32_t sse2_diff_or_zero_mask_16(const char* a, const char* b) {
    uint32_t m;
    __asm__ volatile (
        "pxor    %%xmm0, %%xmm0 \n\t"
        "movdqu  (%1), %%xmm1 \n\t"
        "movdqu  (%2), %%xmm2 \n\t"
        "movdqa  %%xmm1, %%xmm3 \n\t"
        "pcmpeqb %%xmm2, %%xmm3 \n\t"
        "pmovmskb %%xmm3, %%eax \n\t"
        "xor     $0xFFFF, %%eax \n\t"
        "movdqa  %%xmm1, %%xmm4 \n\t"
        "pcmpeqb %%xmm0, %%xmm4 \n\t"
        "movdqa  %%xmm2, %%xmm5 \n\t"
        "pcmpeqb %%xmm0, %%xmm5 \n\t"
        "por     %%xmm5, %%xmm4 \n\t"
        "pmovmskb %%xmm4, %%edx \n\t"
        "or      %%edx, %%eax \n\t"
        : "=a"(m)
        : "r"(a), "r"(b)
        : "edx", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "cc", "memory"
    );
    return m;
}

__attribute__((target("sse2"))) __attribute__((always_inline))
static inline uint32_t sse2_diff_or_zero_mask_16_aligned(const char* a, const char* b) {
    uint32_t m;
    __asm__ volatile (
        "pxor    %%xmm0, %%xmm0 \n\t"
        "movdqa  (%1), %%xmm1 \n\t"
        "movdqa  (%2), %%xmm2 \n\t"
        "movdqa  %%xmm1, %%xmm3 \n\t"
        "pcmpeqb %%xmm2, %%xmm3 \n\t"
        "pmovmskb %%xmm3, %%eax \n\t"
        "xor     $0xFFFF, %%eax \n\t"
        "movdqa  %%xmm1, %%xmm4 \n\t"
        "pcmpeqb %%xmm0, %%xmm4 \n\t"
        "movdqa  %%xmm2, %%xmm5 \n\t"
        "pcmpeqb %%xmm0, %%xmm5 \n\t"
        "por     %%xmm5, %%xmm4 \n\t"
        "pmovmskb %%xmm4, %%edx \n\t"
        "or      %%edx, %%eax \n\t"
        : "=a"(m)
        : "r"(a), "r"(b)
        : "edx", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "cc", "memory"
    );
    return m;
}

__attribute__((target("sse2"))) __attribute__((always_inline))
static inline uint32_t sse2_zero_mask_16_aligned(const char* p) {
    uint32_t m;
    __asm__ volatile (
        "pxor    %%xmm0, %%xmm0 \n\t"
        "movdqa  (%1), %%xmm1 \n\t"
        "pcmpeqb %%xmm0, %%xmm1 \n\t"
        "pmovmskb %%xmm1, %%eax \n\t"
        : "=a"(m)
        : "r"(p)
        : "xmm0", "xmm1", "cc", "memory"
    );
    return m;
}

__attribute__((target("sse2"))) __attribute__((always_inline))
static inline void memcpy_sse_aligned(void* dest, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dest;
    const uint8_t* s = (const uint8_t*)src;

    while (n >= 64) {
        __asm__ volatile (
            "movdqa (%0), %%xmm0 \n\t"
            "movdqa 16(%0), %%xmm1 \n\t"
            "movdqa 32(%0), %%xmm2 \n\t"
            "movdqa 48(%0), %%xmm3 \n\t"
            "movdqa %%xmm0, (%1) \n\t"
            "movdqa %%xmm1, 16(%1) \n\t"
            "movdqa %%xmm2, 32(%1) \n\t"
            "movdqa %%xmm3, 48(%1) \n\t"
            : : "r"(s), "r"(d)
            : "memory", "xmm0", "xmm1", "xmm2", "xmm3"
        );
        s += 64;
        d += 64;
        n -= 64;
    }
}

__attribute__((target("sse2"))) __attribute__((always_inline))
static inline void memcpy_sse_unaligned_src(void* dest, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dest;
    const uint8_t* s = (const uint8_t*)src;

    while (n >= 64) {
        __asm__ volatile (
            "movdqu (%0), %%xmm0 \n\t"
            "movdqu 16(%0), %%xmm1 \n\t"
            "movdqu 32(%0), %%xmm2 \n\t"
            "movdqu 48(%0), %%xmm3 \n\t"
            "movdqa %%xmm0, (%1) \n\t"
            "movdqa %%xmm1, 16(%1) \n\t"
            "movdqa %%xmm2, 32(%1) \n\t"
            "movdqa %%xmm3, 48(%1) \n\t"
            : : "r"(s), "r"(d)
            : "memory", "xmm0", "xmm1", "xmm2", "xmm3"
        );
        s += 64;
        d += 64;
        n -= 64;
    }
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

    while (n && (((uintptr_t)d & 0xFu) != 0u)) {
        *d++ = (uint8_t)val;
        n--;
    }

    while (n >= 64) {
        __asm__ volatile (
            "movdqa %%xmm0, (%0)\n"
            "movdqa %%xmm0, 16(%0)\n"
            "movdqa %%xmm0, 32(%0)\n"
            "movdqa %%xmm0, 48(%0)\n"
            : : "r"(d) : "memory"
        );
        d += 64;
        n -= 64;
    }

    while (n >= 4) {
        *(u32_alias*)d = v;
        d += 4;
        n -= 4;
    }

    while (n--) *d++ = (uint8_t)val;
    return dest;
}

void* memcpy(void* dest, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dest;
    const uint8_t* s = (const uint8_t*)src;
    if (unlikely(n == 0 || d == s)) return dest;

    if (cpu_has_sse2() && unlikely(n >= 64)) {
        while (n && (((uintptr_t)d & 0xFu) != 0u)) {
            *d++ = *s++;
            n--;
        }

        size_t bulk = n & ~(size_t)63;
        if (bulk) {
            if ((((uintptr_t)s & 0xFu) == 0u)) memcpy_sse_aligned(d, s, bulk);
            else memcpy_sse_unaligned_src(d, s, bulk);
            s += bulk;
            d += bulk;
            n -= bulk;
        }
    }

    if (n) {
        size_t dwords = n >> 2;
        size_t bytes = n & 3u;
        if (dwords) {
            __asm__ volatile(
                "cld\n\t"
                "rep movsl"
                : "+D"(d), "+S"(s), "+c"(dwords)
                :
                : "memory", "cc"
            );
        }
        if (bytes) {
            __asm__ volatile(
                "cld\n\t"
                "rep movsb"
                : "+D"(d), "+S"(s), "+c"(bytes)
                :
                : "memory", "cc"
            );
        }
    }
    return dest;
}

void* memmove(void* dest, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dest;
    const uint8_t* s = (const uint8_t*)src;
    if (unlikely(n == 0 || d == s)) return dest;

    if (d < s || (uintptr_t)d >= (uintptr_t)(s + n)) {
        return memcpy(d, s, n);
    }

    uint8_t* d_end = d + n;
    const uint8_t* s_end = s + n;

    if (cpu_has_sse2() && unlikely(n >= 64)) {
        while (n && (((uintptr_t)d_end & 0xFu) != 0u)) {
            *--d_end = *--s_end;
            n--;
        }

        size_t bulk = n & ~(size_t)63;
        if (bulk) {
            int src_aligned = (((uintptr_t)s_end & 0xFu) == 0u);
            size_t rem = bulk;
            while (rem) {
                s_end -= 64;
                d_end -= 64;
                if (src_aligned) {
                    __asm__ volatile (
                        "movdqa (%0), %%xmm0 \n\t"
                        "movdqa 16(%0), %%xmm1 \n\t"
                        "movdqa 32(%0), %%xmm2 \n\t"
                        "movdqa 48(%0), %%xmm3 \n\t"
                        "movdqa %%xmm0, (%1) \n\t"
                        "movdqa %%xmm1, 16(%1) \n\t"
                        "movdqa %%xmm2, 32(%1) \n\t"
                        "movdqa %%xmm3, 48(%1) \n\t"
                        : : "r"(s_end), "r"(d_end)
                        : "memory", "xmm0", "xmm1", "xmm2", "xmm3"
                    );
                } else {
                    __asm__ volatile (
                        "movdqu (%0), %%xmm0 \n\t"
                        "movdqu 16(%0), %%xmm1 \n\t"
                        "movdqu 32(%0), %%xmm2 \n\t"
                        "movdqu 48(%0), %%xmm3 \n\t"
                        "movdqa %%xmm0, (%1) \n\t"
                        "movdqa %%xmm1, 16(%1) \n\t"
                        "movdqa %%xmm2, 32(%1) \n\t"
                        "movdqa %%xmm3, 48(%1) \n\t"
                        : : "r"(s_end), "r"(d_end)
                        : "memory", "xmm0", "xmm1", "xmm2", "xmm3"
                    );
                }
                rem -= 64;
            }
            n -= bulk;
        }
    }

    if (n) {
        size_t bytes = n & 3u;
        while (bytes--) {
            *--d_end = *--s_end;
        }

        size_t dwords = n >> 2;
        if (dwords) {
            d_end -= 4;
            s_end -= 4;
            __asm__ volatile(
                "std\n\t"
                "rep movsl\n\t"
                "cld"
                : "+D"(d_end), "+S"(s_end), "+c"(dwords)
                :
                : "memory", "cc"
            );
        }
    }
    return dest;
}

void* memset(void* s, int c, size_t n) {
    if (n < 64) {
        uint8_t* p = (uint8_t*)s;
        uint8_t cc = (uint8_t)c;

        while (n && ((uintptr_t)p & 3u)) {
            *p++ = cc;
            n--;
        }

        uint32_t vv = (uint32_t)cc;
        vv |= (vv << 8) | (vv << 16) | (vv << 24);
        u32_alias* p32 = (u32_alias*)p;

        while (n >= 16) {
            p32[0] = vv;
            p32[1] = vv;
            p32[2] = vv;
            p32[3] = vv;
            p32 += 4;
            n -= 16;
        }
        while (n >= 4) {
            *p32++ = vv;
            n -= 4;
        }

        p = (uint8_t*)p32;
        while (n--) *p++ = cc;
        return s;
    }
    if (cpu_has_sse2()) return memset_sse(s, c, n);
    uint8_t* p = (uint8_t*)s;
    uint8_t cc = (uint8_t)c;
    while (n--) *p++ = cc;
    return s;
}

int memcmp(const void* s1, const void* s2, size_t n) {
    const uint8_t* p1 = (const uint8_t*)s1;
    const uint8_t* p2 = (const uint8_t*)s2;
    if (unlikely(n == 0 || p1 == p2)) return 0;

    while (n && (((uintptr_t)p1 & 3u) != 0u)) {
        uint8_t a = *p1++;
        uint8_t b = *p2++;
        if (a != b) return (int)a - (int)b;
        n--;
    }

    while (n >= 4) {
        uint32_t a = *(const u32_alias*)p1;
        uint32_t b = *(const u32_alias*)p2;
        if (a != b) {
            for (int i = 0; i < 4; i++) {
                uint8_t ac = p1[i];
                uint8_t bc = p2[i];
                if (ac != bc) return (int)ac - (int)bc;
            }
        }
        p1 += 4;
        p2 += 4;
        n -= 4;
    }

    while (n) {
        uint8_t a = *p1++;
        uint8_t b = *p2++;
        if (a != b) return (int)a - (int)b;
        n--;
    }
    return 0;
}

void* memchr(const void* s, int c, size_t n) {
    const uint8_t* p = (const uint8_t*)s;
    uint8_t cc = (uint8_t)c;
    while (n && (((uintptr_t)p & 3u) != 0u)) {
        if (*p == cc) return (void*)p;
        p++;
        n--;
    }

    uint32_t vv = (uint32_t)cc;
    vv |= (vv << 8) | (vv << 16) | (vv << 24);
    while (n >= 4) {
        uint32_t x = *(const u32_alias*)p;
        uint32_t y = x ^ vv;
        uint32_t m = (y - 0x01010101u) & (~y) & 0x80808080u;
        if (m) {
            for (int i = 0; i < 4; i++) {
                if (p[i] == cc) return (void*)(p + i);
            }
        }
        p += 4;
        n -= 4;
    }
    while (n--) {
        if (*p == cc) return (void*)p;
        p++;
    }
    return NULL;
}

size_t strlen(const char* s) {
    if (!cpu_has_sse2()) {
        size_t len = 0;
        while (s[len]) len++;
        return len;
    }

    const char* start = s;
    while (((uintptr_t)s & 0xFu) != 0u) {
        if (*s == 0) return (size_t)(s - start);
        s++;
    }

    for (;;) {
        if (unlikely(page_off(s) > (PAGE_MASK - 32u))) {
            size_t rem = PAGE_SIZE - (size_t)page_off(s);
            for (size_t i = 0; i < rem; i++) {
                if (s[i] == 0) return (size_t)((s + i) - start);
            }
            s += rem;
            continue;
        }

        uint32_t m0 = sse2_zero_mask_16_aligned(s);
        if (m0) {
            uint32_t idx;
            __asm__ volatile("bsf %1, %0" : "=r"(idx) : "r"(m0) : "cc");
            return (size_t)((s + idx) - start);
        }

        uint32_t m1 = sse2_zero_mask_16_aligned(s + 16);
        if (m1) {
            uint32_t idx;
            __asm__ volatile("bsf %1, %0" : "=r"(idx) : "r"(m1) : "cc");
            return (size_t)((s + 16 + idx) - start);
        }

        s += 32;
    }
}

char* strcpy(char* dest, const char* src) {
    char* d = dest;
    while ((*d++ = *src++));
    return dest;
}

char* strncpy(char* dest, const char* src, size_t n) {
    size_t i;
    for (i = 0; i < n && src[i] != '\0'; i++)
        dest[i] = src[i];
    for (; i < n; i++)
        dest[i] = '\0';
    return dest;
}

char* strcat(char* dest, const char* src) {
    char* ptr = dest + strlen(dest);
    while (*src != '\0') {
        *ptr++ = *src++;
    }
    *ptr = '\0';
    return dest;
}

char* strncat(char* dest, const char* src, size_t n) {
    char* ptr = dest + strlen(dest);
    while (n-- && *src != '\0') {
        *ptr++ = *src++;
    }
    *ptr = '\0';
    return dest;
}

int strcmp(const char* s1, const char* s2) {
    if (!cpu_has_sse2()) {
        while (*s1 && (*s1 == *s2)) {
            s1++;
            s2++;
        }
        return *(const unsigned char*)s1 - *(const unsigned char*)s2;
    }

    const char* a = s1;
    const char* b = s2;

    if ((((uintptr_t)a ^ (uintptr_t)b) & 0xFu) == 0u) {
        while (((uintptr_t)a & 0xFu) != 0u) {
            unsigned char ac = (unsigned char)*a;
            unsigned char bc = (unsigned char)*b;
            if (ac != bc) return (int)ac - (int)bc;
            if (!ac) return 0;
            a++;
            b++;
        }

        for (;;) {
            if (unlikely(page_off(a) > (PAGE_MASK - 32u) || page_off(b) > (PAGE_MASK - 32u))) {
                size_t rem_a = PAGE_SIZE - (size_t)page_off(a);
                size_t rem_b = PAGE_SIZE - (size_t)page_off(b);
                size_t rem = (rem_a < rem_b) ? rem_a : rem_b;
                for (size_t i = 0; i < rem; i++) {
                    unsigned char ac = (unsigned char)a[i];
                    unsigned char bc = (unsigned char)b[i];
                    if (ac != bc) return (int)ac - (int)bc;
                    if (!ac) return 0;
                }
                a += rem;
                b += rem;
                continue;
            }

            uint32_t m0 = sse2_diff_or_zero_mask_16_aligned(a, b);
            if (!m0) {
                uint32_t m1 = sse2_diff_or_zero_mask_16_aligned(a + 16, b + 16);
                if (!m1) {
                    a += 32;
                    b += 32;
                    continue;
                }
                uint32_t idx;
                __asm__ volatile("bsf %1, %0" : "=r"(idx) : "r"(m1) : "cc");
                idx += 16;
                unsigned char ac = (unsigned char)a[idx];
                unsigned char bc = (unsigned char)b[idx];
                return (int)ac - (int)bc;
            }

            uint32_t idx;
            __asm__ volatile("bsf %1, %0" : "=r"(idx) : "r"(m0) : "cc");
            unsigned char ac = (unsigned char)a[idx];
            unsigned char bc = (unsigned char)b[idx];
            return (int)ac - (int)bc;
        }
    }

    for (;;) {
        if (unlikely(page_off(a) > (PAGE_MASK - 32u) || page_off(b) > (PAGE_MASK - 32u))) {
            size_t rem_a = PAGE_SIZE - (size_t)page_off(a);
            size_t rem_b = PAGE_SIZE - (size_t)page_off(b);
            size_t rem = (rem_a < rem_b) ? rem_a : rem_b;
            for (size_t i = 0; i < rem; i++) {
                unsigned char ac = (unsigned char)a[i];
                unsigned char bc = (unsigned char)b[i];
                if (ac != bc) return (int)ac - (int)bc;
                if (!ac) return 0;
            }
            a += rem;
            b += rem;
            continue;
        }

        uint32_t m0 = sse2_diff_or_zero_mask_16(a, b);
        if (!m0) {
            uint32_t m1 = sse2_diff_or_zero_mask_16(a + 16, b + 16);
            if (!m1) {
                a += 32;
                b += 32;
                continue;
            }
            uint32_t idx;
            __asm__ volatile("bsf %1, %0" : "=r"(idx) : "r"(m1) : "cc");
            idx += 16;
            unsigned char ac = (unsigned char)a[idx];
            unsigned char bc = (unsigned char)b[idx];
            return (int)ac - (int)bc;
        }

        uint32_t idx;
        __asm__ volatile("bsf %1, %0" : "=r"(idx) : "r"(m0) : "cc");
        unsigned char ac = (unsigned char)a[idx];
        unsigned char bc = (unsigned char)b[idx];
        return (int)ac - (int)bc;
    }
}

int strncmp(const char* s1, const char* s2, size_t n) {
    if (!n) return 0;
    if (!cpu_has_sse2()) {
        while (n && *s1 && (*s1 == *s2)) {
            s1++;
            s2++;
            n--;
        }
        if (n == 0) return 0;
        return *(const unsigned char*)s1 - *(const unsigned char*)s2;
    }

    const char* a = s1;
    const char* b = s2;

    if ((((uintptr_t)a ^ (uintptr_t)b) & 0xFu) == 0u) {
        while (n && (((uintptr_t)a & 0xFu) != 0u)) {
            unsigned char ac = (unsigned char)*a;
            unsigned char bc = (unsigned char)*b;
            if (ac != bc) return (int)ac - (int)bc;
            if (!ac) return 0;
            a++;
            b++;
            n--;
        }

        while (n >= 32) {
            if (unlikely(page_off(a) > (PAGE_MASK - 32u) || page_off(b) > (PAGE_MASK - 32u))) break;

            uint32_t m0 = sse2_diff_or_zero_mask_16_aligned(a, b);
            if (!m0) {
                uint32_t m1 = sse2_diff_or_zero_mask_16_aligned(a + 16, b + 16);
                if (!m1) {
                    a += 32;
                    b += 32;
                    n -= 32;
                    continue;
                }

                uint32_t idx;
                __asm__ volatile("bsf %1, %0" : "=r"(idx) : "r"(m1) : "cc");
                idx += 16;
                if ((size_t)idx >= n) return 0;

                unsigned char ac = (unsigned char)a[idx];
                unsigned char bc = (unsigned char)b[idx];
                return (int)ac - (int)bc;
            }

            uint32_t idx;
            __asm__ volatile("bsf %1, %0" : "=r"(idx) : "r"(m0) : "cc");
            if ((size_t)idx >= n) return 0;

            unsigned char ac = (unsigned char)a[idx];
            unsigned char bc = (unsigned char)b[idx];
            return (int)ac - (int)bc;
        }

        while (n >= 16) {
            if (unlikely(page_off(a) > (PAGE_MASK - 16u) || page_off(b) > (PAGE_MASK - 16u))) break;

            uint32_t m = sse2_diff_or_zero_mask_16_aligned(a, b);
            if (!m) {
                a += 16;
                b += 16;
                n -= 16;
                continue;
            }

            uint32_t idx;
            __asm__ volatile("bsf %1, %0" : "=r"(idx) : "r"(m) : "cc");
            if ((size_t)idx >= n) return 0;

            unsigned char ac = (unsigned char)a[idx];
            unsigned char bc = (unsigned char)b[idx];
            return (int)ac - (int)bc;
        }
    } else {
        while (n >= 32) {
            if (unlikely(page_off(a) > (PAGE_MASK - 32u) || page_off(b) > (PAGE_MASK - 32u))) break;

            uint32_t m0 = sse2_diff_or_zero_mask_16(a, b);
            if (!m0) {
                uint32_t m1 = sse2_diff_or_zero_mask_16(a + 16, b + 16);
                if (!m1) {
                    a += 32;
                    b += 32;
                    n -= 32;
                    continue;
                }

                uint32_t idx;
                __asm__ volatile("bsf %1, %0" : "=r"(idx) : "r"(m1) : "cc");
                idx += 16;
                if ((size_t)idx >= n) return 0;

                unsigned char ac = (unsigned char)a[idx];
                unsigned char bc = (unsigned char)b[idx];
                return (int)ac - (int)bc;
            }

            uint32_t idx;
            __asm__ volatile("bsf %1, %0" : "=r"(idx) : "r"(m0) : "cc");
            if ((size_t)idx >= n) return 0;

            unsigned char ac = (unsigned char)a[idx];
            unsigned char bc = (unsigned char)b[idx];
            return (int)ac - (int)bc;
        }

        while (n >= 16) {
            if (unlikely(page_off(a) > (PAGE_MASK - 16u) || page_off(b) > (PAGE_MASK - 16u))) break;

            uint32_t m = sse2_diff_or_zero_mask_16(a, b);
            if (!m) {
                a += 16;
                b += 16;
                n -= 16;
                continue;
            }

            uint32_t idx;
            __asm__ volatile("bsf %1, %0" : "=r"(idx) : "r"(m) : "cc");
            if ((size_t)idx >= n) return 0;

            unsigned char ac = (unsigned char)a[idx];
            unsigned char bc = (unsigned char)b[idx];
            return (int)ac - (int)bc;
        }
    }

    for (size_t i = 0; i < n; i++) {
        unsigned char ac = (unsigned char)a[i];
        unsigned char bc = (unsigned char)b[i];
        if (ac != bc) return (int)ac - (int)bc;
        if (!ac) return 0;
    }
    return 0;
}

char* strchr(const char* s, int c) {
    while (*s != (char)c) {
        if (!*s++) {
            return NULL;
        }
    }
    return (char*)s;
}

char* strrchr(const char* s, int c) {
    const char* last = NULL;
    do {
        if (*s == (char)c) {
            last = s;
        }
    } while (*s++);
    return (char*)last;
}

char* strstr(const char* haystack, const char* needle) {
    if (!*needle) return (char*)haystack;
    for (; *haystack; haystack++) {
        if (*haystack == *needle) {
            const char* h = haystack;
            const char* n = needle;
            while (*h && *n && *h == *n) {
                h++;
                n++;
            }
            if (!*n) return (char*)haystack;
        }
    }
    return NULL;
}

char* strpbrk(const char* s, const char* accept) {
    while (*s) {
        const char* a = accept;
        while (*a) {
            if (*a++ == *s) return (char*)s;
        }
        s++;
    }
    return NULL;
}

char* strdup(const char* s) {
    size_t len = strlen(s) + 1;
    char* new_str = (char*)malloc(len);
    if (new_str) {
        memcpy(new_str, s, len);
    }
    return new_str;
}

char* strrev(char* s) {
    int i = 0;
    int j = strlen(s) - 1;
    while (i < j) {
        char c = s[i];
        s[i] = s[j];
        s[j] = c;
        i++;
        j--;
    }
    return s;
}