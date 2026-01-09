// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include <stdint.h>
#include <hal/simd.h>

#include "string.h"

    
#if defined(__GNUC__) || defined(__clang__)
    #define likely(x)       __builtin_expect(!!(x), 1)
    #define unlikely(x)     __builtin_expect(!!(x), 0)
#else
    #define likely(x)       (x)
    #define unlikely(x)     (x)
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

__attribute__((target("sse2")))
int strcmp(const char* a, const char* b) {
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

__attribute__((target("sse2")))
size_t strlen(const char* s) {
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

__attribute__((target("sse2"))) __attribute__((always_inline))
static inline size_t strnlen_sse2(const char* s, size_t max) {
    const char* p = s;
    size_t n = max;

    while (n && (((uintptr_t)p & 0xFu) != 0u)) {
        if (!*p) return (size_t)(p - s);
        p++;
        n--;
    }

    if (!n) return max;

    while (n) {
        if (unlikely(page_off(p) > (PAGE_MASK - 16u))) {
            size_t rem = PAGE_SIZE - (size_t)page_off(p);
            if (rem > n) rem = n;
            for (size_t i = 0; i < rem; i++) {
                if (!p[i]) return (size_t)((p + i) - s);
            }
            p += rem;
            n -= rem;
            continue;
        }

        if (n >= 16) {
            uint32_t m = sse2_zero_mask_16_aligned(p);
            if (m) {
                uint32_t idx;
                __asm__ volatile("bsf %1, %0" : "=r"(idx) : "r"(m) : "cc");
                return (size_t)((p + idx) - s);
            }
            p += 16;
            n -= 16;
            continue;
        }

        for (size_t i = 0; i < n; i++) {
            if (!p[i]) return (size_t)((p + i) - s);
        }
        return max;
    }

    return max;
}

__attribute__((target("sse2")))
int strncmp(const char* a, const char* b, size_t n) {
    if (!n) return 0;

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

size_t strlcpy(char* restrict dst, const char* restrict src, size_t dstsz) {
    size_t slen = strlen(src);
    if (!dstsz) return slen;

    size_t to_copy = slen;
    if (to_copy >= dstsz) to_copy = dstsz - 1;
    if (to_copy) memcpy(dst, src, to_copy);
    dst[to_copy] = 0;
    return slen;
}

__attribute__((target("sse2")))
size_t strlcat(char* restrict dst, const char* restrict src, size_t dstsz) {
    if (!dstsz) return strlen(src);

    size_t dlen = strnlen_sse2(dst, dstsz);

    size_t slen = strlen(src);
    if (dlen == dstsz) return dstsz + slen;

    size_t to_copy = dstsz - dlen - 1;
    if (to_copy) {
        if (slen < to_copy) to_copy = slen;
        if (to_copy) memcpy(dst + dlen, src, to_copy);
        dst[dlen + to_copy] = '\0';
    }

    return dlen + slen;
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
        s += 64; d += 64; n -= 64;
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
        s += 64; d += 64; n -= 64;
    }
}

__attribute__((target("avx"))) __attribute__((always_inline))
static inline void memcpy_avx_aligned_src(void* dest, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dest;
    const uint8_t* s = (const uint8_t*)src;

    while (n >= 128) {
        __asm__ volatile (
            "prefetchnta 256(%0) \n\t"
            "vmovdqa   (%0), %%ymm0 \n\t"
            "vmovdqa 32(%0), %%ymm1 \n\t"
            "vmovdqa 64(%0), %%ymm2 \n\t"
            "vmovdqa 96(%0), %%ymm3 \n\t"
            "vmovdqa %%ymm0,   (%1) \n\t"
            "vmovdqa %%ymm1, 32(%1) \n\t"
            "vmovdqa %%ymm2, 64(%1) \n\t"
            "vmovdqa %%ymm3, 96(%1) \n\t"
            : : "r"(s), "r"(d)
            : "memory", "ymm0", "ymm1", "ymm2", "ymm3"
        );
        s += 128; d += 128; n -= 128;
    }

    __asm__ volatile("vzeroupper" ::: "memory");
}

__attribute__((target("avx"))) __attribute__((always_inline))
static inline void memcpy_avx_unaligned_src(void* dest, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dest;
    const uint8_t* s = (const uint8_t*)src;

    while (n >= 128) {
        __asm__ volatile (
            "prefetchnta 256(%0) \n\t"
            "vmovdqu   (%0), %%ymm0 \n\t"
            "vmovdqu 32(%0), %%ymm1 \n\t"
            "vmovdqu 64(%0), %%ymm2 \n\t"
            "vmovdqu 96(%0), %%ymm3 \n\t"
            "vmovdqa %%ymm0,   (%1) \n\t"
            "vmovdqa %%ymm1, 32(%1) \n\t"
            "vmovdqa %%ymm2, 64(%1) \n\t"
            "vmovdqa %%ymm3, 96(%1) \n\t"
            : : "r"(s), "r"(d)
            : "memory", "ymm0", "ymm1", "ymm2", "ymm3"
        );
        s += 128; d += 128; n -= 128;
    }

    __asm__ volatile("vzeroupper" ::: "memory");
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
        d += 64; n -= 64;
    }

    while (n >= 4) {
        *(u32_alias*)d = v;
        d += 4;
        n -= 4;
    }

    while (n--) *d++ = (uint8_t)val;
    return dest;
}

__attribute__((target("avx"))) __attribute__((target("sse2")))
void* memcpy(void *restrict dst, const void *restrict src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;

    if (unlikely(n >= 128) && simd_can_use_avx()) {
        while (n && (((uintptr_t)d & 0x1Fu) != 0u)) {
            *d++ = *s++;
            n--;
        }

        size_t bulk = n & ~(size_t)127;
        if (bulk) {
            if ((((uintptr_t)s & 0x1Fu) == 0u)) memcpy_avx_aligned_src(d, s, bulk);
            else memcpy_avx_unaligned_src(d, s, bulk);
            s += bulk;
            d += bulk;
            n -= bulk;
        }
    }

    if (unlikely(n >= 64)) {
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

    return dst;
}

__attribute__((target("sse2")))
void* memmove(void* dst, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;

    if (unlikely(n == 0 || d == s)) return dst;

    if (d < s || (uintptr_t)d >= (uintptr_t)(s + n)) {
        return memcpy(d, s, n);
    }

    uint8_t* d_end = d + n;
    const uint8_t* s_end = s + n;

    if (unlikely(n >= 64)) {
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

    return dst;
}

__attribute__((target("sse2")))
void* memset(void* dst, int v, size_t n) {
    if (n < 64) {
        uint8_t* p = (uint8_t*)dst;
        uint8_t c = (uint8_t)v;

        while (n && ((uintptr_t)p & 3u)) {
            *p++ = c;
            n--;
        }

        uint32_t vv = (uint32_t)c;
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
        while (n--) *p++ = c;
        return dst;
    }
    return memset_sse(dst, v, n);
}