// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#ifndef HAL_SIMD_H
#define HAL_SIMD_H

#include <stdint.h>

extern volatile uint32_t kernel_simd_caps;
extern volatile uint64_t kernel_xsave_mask;

#define SIMD_CAP_USE_XSAVE (1u << 0)
#define SIMD_CAP_AVX       (1u << 1)
#define SIMD_CAP_AVX2      (1u << 2)

static inline int simd_can_use_avx(void) {
    return (kernel_simd_caps & SIMD_CAP_AVX) != 0;
}

static inline int simd_can_use_avx2(void) {
    return (kernel_simd_caps & SIMD_CAP_AVX2) != 0;
}

static inline void simd_cpuid(uint32_t leaf, uint32_t subleaf, uint32_t* a, uint32_t* b, uint32_t* c, uint32_t* d) {
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(subleaf));
}

static inline int simd_cpu_has_xsave(void) {
    uint32_t a, b, c, d;
    simd_cpuid(1, 0, &a, &b, &c, &d);
    return (c & (1u << 26)) != 0;
}

static inline int simd_cpu_has_osxsave(void) {
    uint32_t a, b, c, d;
    simd_cpuid(1, 0, &a, &b, &c, &d);
    return (c & (1u << 27)) != 0;
}

static inline int simd_cpu_has_avx(void) {
    uint32_t a, b, c, d;
    simd_cpuid(1, 0, &a, &b, &c, &d);
    return (c & (1u << 28)) != 0;
}

static inline int simd_osxsave_enabled(void) {
    uint32_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    return (cr4 & (1u << 18)) != 0;
}

static inline uint64_t simd_xgetbv(uint32_t index) {
    uint32_t eax, edx;
    __asm__ volatile(".byte 0x0f, 0x01, 0xd0" : "=a"(eax), "=d"(edx) : "c"(index));
    return ((uint64_t)edx << 32) | eax;
}

static inline void simd_xsetbv(uint32_t index, uint64_t value) {
    uint32_t eax = (uint32_t)value;
    uint32_t edx = (uint32_t)(value >> 32);
    __asm__ volatile(".byte 0x0f, 0x01, 0xd1" : : "c"(index), "a"(eax), "d"(edx));
}

static inline uint64_t simd_xcr0_supported_mask(void) {
    uint32_t max_leaf, b, c, d;
    simd_cpuid(0, 0, &max_leaf, &b, &c, &d);
    if (max_leaf < 0xD) return 0;

    uint32_t eax, ebx, ecx, edx;
    simd_cpuid(0xD, 0, &eax, &ebx, &ecx, &edx);
    return ((uint64_t)edx << 32) | eax;
}

static inline uint32_t fpu_state_size(void) {
    if (!simd_cpu_has_xsave()) return 512;

    uint32_t max_leaf, b, c, d;
    simd_cpuid(0, 0, &max_leaf, &b, &c, &d);
    if (max_leaf < 0xD) return 512;

    uint32_t eax, ebx, ecx, edx;
    simd_cpuid(0xD, 0, &eax, &ebx, &ecx, &edx);
    if (ebx < 512) ebx = 512;
    ebx = (ebx + 63u) & ~63u;
    return ebx;
}

static inline void kernel_enable_sse(void) {
    uint32_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~((1u << 2) | (1u << 3));
    cr0 |= (1u << 1);
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0));

    uint32_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1u << 9);
    cr4 |= (1u << 10);
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4));
}

static inline void kernel_init_simd(void) {
    uint32_t a1, b1, c1, d1;
    simd_cpuid(1, 0, &a1, &b1, &c1, &d1);
    int has_xsave = (c1 & (1u << 26)) != 0;
    int has_avx = (c1 & (1u << 28)) != 0;

    kernel_enable_sse();

    uint32_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));

    if (has_xsave) {
        cr4 |= (1u << 18);
    }
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4));

    simd_cpuid(1, 0, &a1, &b1, &c1, &d1);
    int has_osxsave = (c1 & (1u << 27)) != 0;

    kernel_simd_caps = 0;
    kernel_xsave_mask = 0;

    if (has_xsave && has_osxsave) {
        uint64_t supported = simd_xcr0_supported_mask();
        uint64_t xcr0 = 0x3;

        if (has_avx) {
            xcr0 |= 0x4;
        }

        if (supported) {
            xcr0 &= supported;
        }

        simd_xsetbv(0, xcr0);
        kernel_xsave_mask = xcr0;
        kernel_simd_caps |= SIMD_CAP_USE_XSAVE;

        if (((xcr0 & 0x6ull) == 0x6ull) && has_avx) {
            kernel_simd_caps |= SIMD_CAP_AVX;
            
            uint32_t max_leaf, b0, c0, d0;
            simd_cpuid(0, 0, &max_leaf, &b0, &c0, &d0);
            if (max_leaf >= 7) {
                uint32_t a7, b7, c7, d7;
                simd_cpuid(7, 0, &a7, &b7, &c7, &d7);
                if ((b7 & (1u << 5)) != 0u) {
                    kernel_simd_caps |= SIMD_CAP_AVX2;
                }
            }
        }
    }
}

static inline void fpu_save(uint8_t* buffer) {
    if ((kernel_simd_caps & SIMD_CAP_USE_XSAVE) != 0) {
        uint64_t mask = kernel_xsave_mask;
        uint32_t eax = (uint32_t)mask;
        uint32_t edx = (uint32_t)(mask >> 32);
        __asm__ volatile(
            ".byte 0x0f, 0xae, 0x27" 
            :
            : "D"(buffer), "a"(eax), "d"(edx)
            : "memory"
        );
        return;
    }

    __asm__ volatile("fxsave (%0)" : : "r"(buffer) : "memory");
}

static inline void fpu_restore(uint8_t* buffer) {
    if ((kernel_simd_caps & SIMD_CAP_USE_XSAVE) != 0) {
        uint64_t mask = kernel_xsave_mask;
        uint32_t eax = (uint32_t)mask;
        uint32_t edx = (uint32_t)(mask >> 32);
        __asm__ volatile(
            ".byte 0x0f, 0xae, 0x2f" 
            :
            : "D"(buffer), "a"(eax), "d"(edx)
            : "memory"
        );
        return;
    }

    __asm__ volatile("fxrstor (%0)" : : "r"(buffer) : "memory");
}

#endif