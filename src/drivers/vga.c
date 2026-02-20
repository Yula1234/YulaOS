// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include <lib/string.h>
#include <mm/heap.h>
#include <hal/simd.h>
#include <drivers/fbdev.h>
#include <drivers/virtio_gpu.h>

#include "font8x16.h"
#include "vga.h"

static inline int vga_get_hw_fb(uint32_t** out_ptr, uint32_t* out_pitch, uint32_t* out_w, uint32_t* out_h) {
    if (out_ptr) *out_ptr = 0;
    if (out_pitch) *out_pitch = 0;
    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;

    if (virtio_gpu_is_active()) {
        const virtio_gpu_fb_t* fb = virtio_gpu_get_fb();
        if (!fb || !fb->fb_ptr || fb->width == 0 || fb->height == 0 || fb->pitch == 0) return 0;
        if (out_ptr) *out_ptr = fb->fb_ptr;
        if (out_pitch) *out_pitch = fb->pitch;
        if (out_w) *out_w = fb->width;
        if (out_h) *out_h = fb->height;
        return 1;
    }

    if (!fb_ptr || fb_width == 0 || fb_height == 0 || fb_pitch == 0) return 0;
    if (out_ptr) *out_ptr = fb_ptr;
    if (out_pitch) *out_pitch = fb_pitch;
    if (out_w) *out_w = fb_width;
    if (out_h) *out_h = fb_height;
    return 1;
}

uint32_t* back_buffer;

static uint32_t fg_color = COLOR_WHITE;
static uint32_t bg_color = 0x000000;

static uint32_t* vga_current_target = 0;
static uint32_t  vga_target_w = 1024;
static uint32_t  vga_target_h = 768;

int dirty_x1, dirty_y1, dirty_x2, dirty_y2;

static inline int vga_can_use_avx(void);
static inline int vga_can_use_avx2(void);

void vga_reset_dirty() {
    dirty_x1 = 2000;
    dirty_y1 = 2000;
    dirty_x2 = -2000; 
    dirty_y2 = -2000;
}

void vga_mark_dirty(int x, int y, int w, int h) {
    int x1 = x;
    int y1 = y;
    int x2 = x + w;
    int y2 = y + h;

    x1 &= ~3; 
    x2 = (x2 + 3) & ~3;

    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > (int)fb_width)  x2 = (int)fb_width;
    if (y2 > (int)fb_height) y2 = (int)fb_height;

    if (x1 >= x2 || y1 >= y2) return;

    if (x1 < dirty_x1) dirty_x1 = x1;
    if (y1 < dirty_y1) dirty_y1 = y1;
    if (x2 > dirty_x2) dirty_x2 = x2;
    if (y2 > dirty_y2) dirty_y2 = y2;
}

int vga_is_rect_dirty(int x, int y, int w, int h) {
    return !(x + w < dirty_x1 || x > dirty_x2 || y + h < dirty_y1 || y > dirty_y2);
}

void vga_set_target(uint32_t* target, uint32_t w, uint32_t h) {
    if (target == 0) {
        vga_current_target = (back_buffer != 0) ? back_buffer : fb_ptr;
        vga_target_w = fb_width;
        vga_target_h = fb_height;
    } else {
        vga_current_target = target;
        vga_target_w = w;
        vga_target_h = h;
    }
}

typedef union {
    uint32_t u32[4];
    uint8_t  bytes[16];
} __attribute__((aligned(16))) vec128_t;

static const vec128_t font_mask_high = { .u32 = { 0x00000080, 0x00000040, 0x00000020, 0x00000010 } };
static const vec128_t font_mask_low  = { .u32 = { 0x00000008, 0x00000004, 0x00000002, 0x00000001 } };

__attribute__((target("sse2")))
void vga_draw_char_sse(int x, int y, char c, uint32_t fg) {
    if ((uint8_t)c > 127 || !vga_current_target) return;
    if (x < 0 || y < 0 || x + 8 > (int)vga_target_w || y + 16 > (int)vga_target_h) return;

    const uint8_t* glyph = font8x16_basic[(uint8_t)c];
    uint32_t* dst_ptr = &vga_current_target[y * vga_target_w + x];
    int stride_bytes = vga_target_w * 4;

    __asm__ volatile (
        "movd      %[fg], %%xmm0 \n\t"
        "pshufd    $0x00, %%xmm0, %%xmm0 \n\t"
        "pxor      %%xmm7, %%xmm7 \n\t"
        
        "movdqa    %[m_hi], %%xmm5 \n\t" 
        "movdqa    %[m_lo], %%xmm6 \n\t"
        
        "mov       $16, %%ecx \n\t"

        "1: \n\t"
        "movzbl    (%[glyph]), %%eax \n\t"
        "test      %%eax, %%eax \n\t"
        "jz        2f \n\t"

        "movd      %%eax, %%xmm1 \n\t"
        "pshufd    $0x00, %%xmm1, %%xmm1 \n\t"

        "movdqa    %%xmm1, %%xmm2 \n\t"
        "pand      %%xmm5, %%xmm2 \n\t"
        "pcmpgtd   %%xmm7, %%xmm2 \n\t"
        
        "movdqu    (%[dst]), %%xmm3 \n\t"
        "pandn     %%xmm3, %%xmm2 \n\t"
        
        "movdqa    %%xmm1, %%xmm4 \n\t"
        "pand      %%xmm5, %%xmm4 \n\t"
        "pcmpgtd   %%xmm7, %%xmm4 \n\t"
        "pand      %%xmm0, %%xmm4 \n\t"
        
        "por       %%xmm4, %%xmm2 \n\t"
        "movdqu    %%xmm2, (%[dst]) \n\t"

        "movdqa    %%xmm1, %%xmm2 \n\t"
        "pand      %%xmm6, %%xmm2 \n\t"
        "pcmpgtd   %%xmm7, %%xmm2 \n\t"
        
        "movdqu    16(%[dst]), %%xmm3 \n\t"
        "pandn     %%xmm3, %%xmm2 \n\t"
        
        "movdqa    %%xmm1, %%xmm4 \n\t"
        "pand      %%xmm6, %%xmm4 \n\t"
        "pcmpgtd   %%xmm7, %%xmm4 \n\t"
        "pand      %%xmm0, %%xmm4 \n\t"
        
        "por       %%xmm4, %%xmm2 \n\t"
        "movdqu    %%xmm2, 16(%[dst]) \n\t"

        "2: \n\t"
        "inc       %[glyph] \n\t"
        "add       %[stride], %[dst] \n\t"
        "dec       %%ecx \n\t"
        "jnz       1b \n\t"

        : [dst]    "+r" (dst_ptr), 
          [glyph]  "+r" (glyph)
        : [fg]     "m"  (fg), 
          [stride] "m"  (stride_bytes),
          [m_hi]   "m"  (font_mask_high), 
          [m_lo]   "m"  (font_mask_low)
        : "memory", "eax", "ecx", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7"
    );
}

enum {
    VGA_DEBUG_TERM_W = 80,
    VGA_DEBUG_TERM_H = 12,
};

static char term_buffer[VGA_DEBUG_TERM_W * VGA_DEBUG_TERM_H];
static int term_col = 0;
static int term_row = 0;

void vga_init() {
    memset(term_buffer, ' ', sizeof(term_buffer));
    vga_set_target(0, 0, 0);
}

__attribute__((target("sse2")))
void vga_clear(uint32_t color) {
    if (!vga_current_target) return;

    uint32_t pixels = vga_target_w * vga_target_h;
    uint32_t count = pixels / 4;
    uint32_t* ptr = vga_current_target;

    __asm__ volatile (
        "movd %0, %%xmm0\n\t"
        "pshufd $0, %%xmm0, %%xmm0\n\t"
        ".loop_clear_fast:\n\t"
        "movntdq %%xmm0, (%1)\n\t" 
        "add $16, %1\n\t"
        "dec %2\n\t"
        "jnz .loop_clear_fast\n\t"
        "sfence\n\t"
        : : "r"(color), "r"(ptr), "r"(count) : "memory", "xmm0", "cc"
    );
}

void vga_set_color(uint32_t fg, uint32_t bg) {
    fg_color = fg;
    bg_color = bg;
}

void vga_putc(char c) {
    if (c == '\n') {
        term_col = 0; term_row++;
    } else if (c == '\b') {
        if (term_col > 0) term_col--;
        term_buffer[term_row * VGA_DEBUG_TERM_W + term_col] = ' ';
    } else {
        term_buffer[term_row * VGA_DEBUG_TERM_W + term_col] = c;
        term_col++;
    }
    
    if (term_col >= VGA_DEBUG_TERM_W) { term_col = 0; term_row++; }
    if (term_row >= VGA_DEBUG_TERM_H) {
        memmove(term_buffer, term_buffer + VGA_DEBUG_TERM_W, VGA_DEBUG_TERM_W * (VGA_DEBUG_TERM_H - 1));
        memset(term_buffer + VGA_DEBUG_TERM_W * (VGA_DEBUG_TERM_H - 1), ' ', VGA_DEBUG_TERM_W);
        term_row = VGA_DEBUG_TERM_H - 1;
    }
}

void vga_render_terminal(int win_x, int win_y) {
    for (int y = 0; y < VGA_DEBUG_TERM_H; y++) {
        for (int x = 0; x < VGA_DEBUG_TERM_W; x++) {
            char c = term_buffer[y * VGA_DEBUG_TERM_W + x];
            if (c != ' ') {
                vga_draw_char_sse(win_x + x * 8, win_y + y * 16, c, 0x1E1E1E);
            }
        }
    }
}

void vga_clear_terminal() {
    memset(term_buffer, ' ', VGA_DEBUG_TERM_W * VGA_DEBUG_TERM_H);
    
    term_col = 0;
    term_row = 0;
}

void vga_print(const char* s) {
    while (*s) vga_putc(*s++);
}

__attribute__((target("sse2"))) __attribute__((always_inline))
static inline void vga_draw_rect_sse2_impl(int x1, int y1, int draw_w, int draw_h, uint32_t color) {
    const int stride = (int)vga_target_w;
    uint32_t* row = vga_current_target + y1 * stride + x1;

    for (int cy = 0; cy < draw_h; cy++) {
        uint32_t* dst32 = row;
        int n = draw_w;

        while (n && (((uintptr_t)dst32 & 0xFu) != 0u)) {
            *dst32++ = color;
            n--;
        }

        uint8_t* p = (uint8_t*)dst32;
        size_t blocks16 = (size_t)n >> 2;
        size_t blocks64 = blocks16 >> 2;
        size_t rem16 = blocks16 & 3u;

        if (blocks64 | rem16) {
            size_t tmp64 = blocks64;
            size_t tmp16 = rem16;
            __asm__ volatile (
                "movd %[color], %%xmm0\n\t"
                "pshufd $0, %%xmm0, %%xmm0\n\t"

                "test %[b64], %[b64]\n\t"
                "jz 2f\n\t"
                "1:\n\t"
                "movdqa %%xmm0,   0(%[p])\n\t"
                "movdqa %%xmm0,  16(%[p])\n\t"
                "movdqa %%xmm0,  32(%[p])\n\t"
                "movdqa %%xmm0,  48(%[p])\n\t"
                "add $64, %[p]\n\t"
                "dec %[b64]\n\t"
                "jnz 1b\n\t"
                "2:\n\t"

                "test %[b16], %[b16]\n\t"
                "jz 4f\n\t"
                "3:\n\t"
                "movdqa %%xmm0, (%[p])\n\t"
                "add $16, %[p]\n\t"
                "dec %[b16]\n\t"
                "jnz 3b\n\t"
                "4:\n\t"
                : [p]"+r"(p), [b64]"+r"(tmp64), [b16]"+r"(tmp16)
                : [color]"r"(color)
                : "memory", "xmm0", "cc"
            );
        }

        dst32 = (uint32_t*)p;
        for (int i = 0, tail = (n & 3); i < tail; i++) dst32[i] = color;

        row += stride;
    }
}

__attribute__((target("avx")))
static void vga_draw_rect_avx_impl(int x1, int y1, int draw_w, int draw_h, uint32_t color) {
    const int stride = (int)vga_target_w;
    uint32_t* row = vga_current_target + y1 * stride + x1;

    for (int cy = 0; cy < draw_h; cy++) {
        uint32_t* dst32 = row;
        int n = draw_w;

        while (n && (((uintptr_t)dst32 & 0x1Fu) != 0u)) {
            *dst32++ = color;
            n--;
        }

        uint8_t* p = (uint8_t*)dst32;
        size_t blocks32 = (size_t)n >> 3;
        size_t blocks128 = blocks32 >> 2;
        size_t rem32 = blocks32 & 3u;

        if (blocks128 | rem32) {
            size_t tmp128 = blocks128;
            size_t tmp32 = rem32;
            __asm__ volatile (
                "vmovd %[color], %%xmm0\n\t"
                "vpshufd $0, %%xmm0, %%xmm0\n\t"
                "vxorps %%ymm1, %%ymm1, %%ymm1\n\t"
                
                "vinsertf128 $0, %%xmm0, %%ymm1, %%ymm0\n\t"
                "vinsertf128 $1, %%xmm0, %%ymm0, %%ymm0\n\t"

                "test %[b128], %[b128]\n\t"
                "jz 2f\n\t"
                "1:\n\t"
                "vmovdqa %%ymm0,   0(%[p])\n\t"
                "vmovdqa %%ymm0,  32(%[p])\n\t"
                "vmovdqa %%ymm0,  64(%[p])\n\t"
                "vmovdqa %%ymm0,  96(%[p])\n\t"
                "add $128, %[p]\n\t"
                "dec %[b128]\n\t"
                "jnz 1b\n\t"
                "2:\n\t"

                "test %[b32], %[b32]\n\t"
                "jz 4f\n\t"
                "3:\n\t"
                "vmovdqa %%ymm0, (%[p])\n\t"
                "add $32, %[p]\n\t"
                "dec %[b32]\n\t"
                "jnz 3b\n\t"
                "4:\n\t"
                : [p]"+r"(p), [b128]"+r"(tmp128), [b32]"+r"(tmp32)
                : [color]"r"(color)
                : "memory", "xmm0", "ymm0", "ymm1", "cc"
            );
        }

        dst32 = (uint32_t*)p;
        for (int i = 0, tail = (n & 7); i < tail; i++) dst32[i] = color;

        row += stride;
    }

    __asm__ volatile ("vzeroupper" ::: "memory");
}

__attribute__((target("sse2")))
void vga_draw_rect(int x, int y, int w, int h, uint32_t color) {
    if (!vga_current_target) return;
    if (x >= (int)vga_target_w || y >= (int)vga_target_h) return;
    if (x + w < 0 || y + h < 0) return;

    int x1 = (x < 0) ? 0 : x;
    int y1 = (y < 0) ? 0 : y;
    int x2 = (x + w > (int)vga_target_w) ? (int)vga_target_w : x + w;
    int y2 = (y + h > (int)vga_target_h) ? (int)vga_target_h : y + h;

    int draw_w = x2 - x1;
    int draw_h = y2 - y1;
    if (draw_w <= 0 || draw_h <= 0) return;

    if (vga_can_use_avx()) {
        vga_draw_rect_avx_impl(x1, y1, draw_w, draw_h, color);
    } else {
        vga_draw_rect_sse2_impl(x1, y1, draw_w, draw_h, color);
    }

    vga_mark_dirty(x, y, w, h);
}

void vga_init_graphics() {
    back_buffer = (uint32_t*)kmalloc_a(fb_width * fb_height * 4);
    vga_set_target(0, 0, 0); 
    
    vga_clear(0x1A1A1B); 
    
    vga_mark_dirty(0, 0, fb_width, fb_height);
    
    vga_flip_dirty(); 
}

void vga_put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (x >= vga_target_w || y >= vga_target_h) return;
    vga_current_target[y * vga_target_w + x] = color;
}

void vga_print_at(const char* s, int x, int y, uint32_t fg) {
    while (*s) {
        vga_draw_char_sse(x + 1, y + 1, *s, 0x000000);
        vga_draw_char_sse(x, y, *s, fg);
        x += 8; 
        s++;
    }
}

__attribute__((target("sse2"))) __attribute__((always_inline)) __attribute__((unused))
static inline void vga_flip_sse2_impl(uint8_t* dst_base, uint32_t dst_pitch, int x1, int x2, int y1, int y2) {
    int width_pixels = x2 - x1;
    if (width_pixels <= 0) return;
 
    size_t row_bytes = (size_t)width_pixels * 4u;
 
    for (int y = y1; y < y2; y++) {
        const uint8_t* s8 = (const uint8_t*)&back_buffer[y * fb_width + x1];
        uint8_t* d8 = dst_base + (size_t)y * (size_t)dst_pitch + (size_t)x1 * 4u;
        size_t bytes = row_bytes;
 
        while (bytes && (((uintptr_t)d8 & 0xFu) != 0u)) {
            *(uint32_t*)d8 = *(const uint32_t*)s8;
            d8 += 4;
            s8 += 4;
            bytes -= 4;
        }
 
        size_t blocks64 = bytes >> 6;
        if (blocks64) {
            if ((((uintptr_t)s8 & 0xFu) == 0u)) {
                __asm__ volatile (
                    "test %2, %2\n\t"
                    "jz 2f\n\t"
                    "1:\n\t"
                    "prefetchnta 256(%0)\n\t"
                    "movdqa   (%0), %%xmm0\n\t"
                    "movdqa 16(%0), %%xmm1\n\t"
                    "movdqa 32(%0), %%xmm2\n\t"
                    "movdqa 48(%0), %%xmm3\n\t"
                    "movntdq %%xmm0,   (%1)\n\t"
                    "movntdq %%xmm1, 16(%1)\n\t"
                    "movntdq %%xmm2, 32(%1)\n\t"
                    "movntdq %%xmm3, 48(%1)\n\t"
                    "add $64, %0\n\t"
                    "add $64, %1\n\t"
                    "dec %2\n\t"
                    "jnz 1b\n\t"
                    "2:\n\t"
                    : "+r"(s8), "+r"(d8), "+r"(blocks64)
                    :
                    : "memory", "xmm0", "xmm1", "xmm2", "xmm3", "cc"
                );
            } else {
                __asm__ volatile (
                    "test %2, %2\n\t"
                    "jz 2f\n\t"
                    "1:\n\t"
                    "prefetchnta 256(%0)\n\t"
                    "movdqu   (%0), %%xmm0\n\t"
                    "movdqu 16(%0), %%xmm1\n\t"
                    "movdqu 32(%0), %%xmm2\n\t"
                    "movdqu 48(%0), %%xmm3\n\t"
                    "movntdq %%xmm0,   (%1)\n\t"
                    "movntdq %%xmm1, 16(%1)\n\t"
                    "movntdq %%xmm2, 32(%1)\n\t"
                    "movntdq %%xmm3, 48(%1)\n\t"
                    "add $64, %0\n\t"
                    "add $64, %1\n\t"
                    "dec %2\n\t"
                    "jnz 1b\n\t"
                    "2:\n\t"
                    : "+r"(s8), "+r"(d8), "+r"(blocks64)
                    :
                    : "memory", "xmm0", "xmm1", "xmm2", "xmm3", "cc"
                );
            }
        }
 
        bytes &= 63u;
        size_t blocks16 = bytes >> 4;
        if (blocks16) {
            if ((((uintptr_t)s8 & 0xFu) == 0u)) {
                __asm__ volatile (
                    "test %2, %2\n\t"
                    "jz 2f\n\t"
                    "1:\n\t"
                    "movdqa (%0), %%xmm0\n\t"
                    "movntdq %%xmm0, (%1)\n\t"
                    "add $16, %0\n\t"
                    "add $16, %1\n\t"
                    "dec %2\n\t"
                    "jnz 1b\n\t"
                    "2:\n\t"
                    : "+r"(s8), "+r"(d8), "+r"(blocks16)
                    :
                    : "memory", "xmm0", "cc"
                );
            } else {
                __asm__ volatile (
                    "test %2, %2\n\t"
                    "jz 2f\n\t"
                    "1:\n\t"
                    "movdqu (%0), %%xmm0\n\t"
                    "movntdq %%xmm0, (%1)\n\t"
                    "add $16, %0\n\t"
                    "add $16, %1\n\t"
                    "dec %2\n\t"
                    "jnz 1b\n\t"
                    "2:\n\t"
                    : "+r"(s8), "+r"(d8), "+r"(blocks16)
                    :
                    : "memory", "xmm0", "cc"
                );
            }
        }
 
        bytes &= 15u;
        while (bytes) {
            *(uint32_t*)d8 = *(const uint32_t*)s8;
            d8 += 4;
            s8 += 4;
            bytes -= 4;
        }
    }
 
    __asm__ volatile ("sfence" ::: "memory");
}

__attribute__((target("avx"))) __attribute__((unused))
static void vga_flip_avx_impl(uint8_t* dst_base, uint32_t dst_pitch, int x1, int x2, int y1, int y2) {
     int width_pixels = x2 - x1;
     if (width_pixels <= 0) return;
 
     size_t row_bytes = (size_t)width_pixels * 4u;
 
     for (int y = y1; y < y2; y++) {
         const uint8_t* s8 = (const uint8_t*)&back_buffer[y * fb_width + x1];
         uint8_t* d8 = dst_base + (size_t)y * (size_t)dst_pitch + (size_t)x1 * 4u;
         size_t bytes = row_bytes;
 
         while (bytes && (((uintptr_t)d8 & 0x1Fu) != 0u)) {
             *(uint32_t*)d8 = *(const uint32_t*)s8;
             d8 += 4;
             s8 += 4;
             bytes -= 4;
         }
 
         size_t blocks128 = bytes >> 7;
         if (blocks128) {
             if ((((uintptr_t)s8 & 0x1Fu) == 0u)) {
                 __asm__ volatile (
                     "test %2, %2\n\t"
                     "jz 2f\n\t"
                     "1:\n\t"
                     "prefetchnta 512(%0)\n\t"
                     "vmovdqa   (%0), %%ymm0\n\t"
                     "vmovdqa  32(%0), %%ymm1\n\t"
                     "vmovdqa  64(%0), %%ymm2\n\t"
                     "vmovdqa  96(%0), %%ymm3\n\t"
                     "vmovntdq %%ymm0,   (%1)\n\t"
                     "vmovntdq %%ymm1,  32(%1)\n\t"
                     "vmovntdq %%ymm2,  64(%1)\n\t"
                     "vmovntdq %%ymm3,  96(%1)\n\t"
                     "add $128, %0\n\t"
                     "add $128, %1\n\t"
                     "dec %2\n\t"
                     "jnz 1b\n\t"
                     "2:\n\t"
                     : "+r"(s8), "+r"(d8), "+r"(blocks128)
                     :
                     : "memory", "ymm0", "ymm1", "ymm2", "ymm3", "cc"
                 );
             } else {
                 __asm__ volatile (
                     "test %2, %2\n\t"
                     "jz 2f\n\t"
                     "1:\n\t"
                     "prefetchnta 512(%0)\n\t"
                     "vmovdqu   (%0), %%ymm0\n\t"
                     "vmovdqu  32(%0), %%ymm1\n\t"
                     "vmovdqu  64(%0), %%ymm2\n\t"
                     "vmovdqu  96(%0), %%ymm3\n\t"
                     "vmovntdq %%ymm0,   (%1)\n\t"
                     "vmovntdq %%ymm1,  32(%1)\n\t"
                     "vmovntdq %%ymm2,  64(%1)\n\t"
                     "vmovntdq %%ymm3,  96(%1)\n\t"
                     "add $128, %0\n\t"
                     "add $128, %1\n\t"
                     "dec %2\n\t"
                     "jnz 1b\n\t"
                     "2:\n\t"
                     : "+r"(s8), "+r"(d8), "+r"(blocks128)
                     :
                     : "memory", "ymm0", "ymm1", "ymm2", "ymm3", "cc"
                 );
             }
         }
 
         bytes &= 127u;
         size_t blocks32 = bytes >> 5;
         if (blocks32) {
             if ((((uintptr_t)s8 & 0x1Fu) == 0u)) {
                 __asm__ volatile (
                     "test %2, %2\n\t"
                     "jz 2f\n\t"
                     "1:\n\t"
                     "vmovdqa (%0), %%ymm0\n\t"
                     "vmovntdq %%ymm0, (%1)\n\t"
                     "add $32, %0\n\t"
                     "add $32, %1\n\t"
                     "dec %2\n\t"
                     "jnz 1b\n\t"
                     "2:\n\t"
                     : "+r"(s8), "+r"(d8), "+r"(blocks32)
                     :
                     : "memory", "ymm0", "cc"
                 );
             } else {
                 __asm__ volatile (
                     "test %2, %2\n\t"
                     "jz 2f\n\t"
                     "1:\n\t"
                     "vmovdqu (%0), %%ymm0\n\t"
                     "vmovntdq %%ymm0, (%1)\n\t"
                     "add $32, %0\n\t"
                     "add $32, %1\n\t"
                     "dec %2\n\t"
                     "jnz 1b\n\t"
                     "2:\n\t"
                     : "+r"(s8), "+r"(d8), "+r"(blocks32)
                     :
                     : "memory", "ymm0", "cc"
                 );
             }
         }
 
         bytes &= 31u;
         size_t blocks16 = bytes >> 4;
         if (blocks16) {
             __asm__ volatile (
                 "test %2, %2\n\t"
                 "jz 2f\n\t"
                 "1:\n\t"
                 "vmovdqu (%0), %%xmm0\n\t"
                 "vmovntdq %%xmm0, (%1)\n\t"
                 "add $16, %0\n\t"
                 "add $16, %1\n\t"
                 "dec %2\n\t"
                 "jnz 1b\n\t"
                 "2:\n\t"
                 : "+r"(s8), "+r"(d8), "+r"(blocks16)
                 :
                 : "memory", "xmm0", "cc"
             );
         }
 
         bytes &= 15u;
         while (bytes) {
             *(uint32_t*)d8 = *(const uint32_t*)s8;
             d8 += 4;
             s8 += 4;
             bytes -= 4;
         }
     }
 
     __asm__ volatile ("sfence" ::: "memory");
     __asm__ volatile ("vzeroupper" ::: "memory");
 }

void vga_draw_cursor(int x, int y) {
    vga_draw_rect(x, y, 5, 5, 0xFFFFFF);
}

__attribute__((unused)) static uint32_t color_blend(uint32_t c1, uint32_t c2, uint8_t alpha) {
    if (alpha == 255) return c1;
    if (alpha == 0) return c2;

    uint32_t rb = ((c1 & 0xFF00FF) * alpha + (c2 & 0xFF00FF) * (256 - alpha)) >> 8;
    uint32_t g  = ((c1 & 0x00FF00) * alpha + (c2 & 0x00FF00) * (256 - alpha)) >> 8;
    return (rb & 0xFF00FF) | (g & 0x00FF00);
}

static inline int vga_can_use_avx(void) {
    return simd_can_use_avx();
}

static inline int vga_can_use_avx2(void) {
    return simd_can_use_avx2();
}
 
__attribute__((target("sse2"))) __attribute__((always_inline))
static inline void vga_flip_dirty_sse2_impl(uint8_t* dst_base, uint32_t dst_pitch, int x1, int x2, int y1, int y2) {
    int width_pixels = x2 - x1;
    if (width_pixels <= 0) return;
 
    size_t row_bytes = (size_t)width_pixels * 4u;
 
    for (int y = y1; y < y2; y++) {
        const uint8_t* s8 = (const uint8_t*)&back_buffer[y * fb_width + x1];
        uint8_t* d8 = dst_base + (size_t)y * (size_t)dst_pitch + (size_t)x1 * 4u;
        size_t bytes = row_bytes;
 
        while (bytes && (((uintptr_t)d8 & 0xFu) != 0u)) {
            *(uint32_t*)d8 = *(const uint32_t*)s8;
            d8 += 4;
            s8 += 4;
            bytes -= 4;
        }
 
        size_t blocks64 = bytes >> 6;
        if (blocks64) {
            if ((((uintptr_t)s8 & 0xFu) == 0u)) {
                __asm__ volatile (
                    "test %2, %2\n\t"
                    "jz 2f\n\t"
                    "1:\n\t"
                    "prefetchnta 256(%0)\n\t"
                    "movdqa   (%0), %%xmm0\n\t"
                    "movdqa 16(%0), %%xmm1\n\t"
                    "movdqa 32(%0), %%xmm2\n\t"
                    "movdqa 48(%0), %%xmm3\n\t"
                    "movntdq %%xmm0,   (%1)\n\t"
                    "movntdq %%xmm1, 16(%1)\n\t"
                    "movntdq %%xmm2, 32(%1)\n\t"
                    "movntdq %%xmm3, 48(%1)\n\t"
                    "add $64, %0\n\t"
                    "add $64, %1\n\t"
                    "dec %2\n\t"
                    "jnz 1b\n\t"
                    "2:\n\t"
                    : "+r"(s8), "+r"(d8), "+r"(blocks64)
                    :
                    : "memory", "xmm0", "xmm1", "xmm2", "xmm3", "cc"
                );
            } else {
                __asm__ volatile (
                    "test %2, %2\n\t"
                    "jz 2f\n\t"
                    "1:\n\t"
                    "prefetchnta 256(%0)\n\t"
                    "movdqu   (%0), %%xmm0\n\t"
                    "movdqu 16(%0), %%xmm1\n\t"
                    "movdqu 32(%0), %%xmm2\n\t"
                    "movdqu 48(%0), %%xmm3\n\t"
                    "movntdq %%xmm0,   (%1)\n\t"
                    "movntdq %%xmm1, 16(%1)\n\t"
                    "movntdq %%xmm2, 32(%1)\n\t"
                    "movntdq %%xmm3, 48(%1)\n\t"
                    "add $64, %0\n\t"
                    "add $64, %1\n\t"
                    "dec %2\n\t"
                    "jnz 1b\n\t"
                    "2:\n\t"
                    : "+r"(s8), "+r"(d8), "+r"(blocks64)
                    :
                    : "memory", "xmm0", "xmm1", "xmm2", "xmm3", "cc"
                );
            }
        }
 
        bytes &= 63u;
        size_t blocks16 = bytes >> 4;
        if (blocks16) {
            if ((((uintptr_t)s8 & 0xFu) == 0u)) {
                __asm__ volatile (
                    "test %2, %2\n\t"
                    "jz 2f\n\t"
                    "1:\n\t"
                    "movdqa (%0), %%xmm0\n\t"
                    "movntdq %%xmm0, (%1)\n\t"
                    "add $16, %0\n\t"
                    "add $16, %1\n\t"
                    "dec %2\n\t"
                    "jnz 1b\n\t"
                    "2:\n\t"
                    : "+r"(s8), "+r"(d8), "+r"(blocks16)
                    :
                    : "memory", "xmm0", "cc"
                );
            } else {
                __asm__ volatile (
                    "test %2, %2\n\t"
                    "jz 2f\n\t"
                    "1:\n\t"
                    "movdqu (%0), %%xmm0\n\t"
                    "movntdq %%xmm0, (%1)\n\t"
                    "add $16, %0\n\t"
                    "add $16, %1\n\t"
                    "dec %2\n\t"
                    "jnz 1b\n\t"
                    "2:\n\t"
                    : "+r"(s8), "+r"(d8), "+r"(blocks16)
                    :
                    : "memory", "xmm0", "cc"
                );
            }
        }
 
        bytes &= 15u;
        while (bytes) {
            *(uint32_t*)d8 = *(const uint32_t*)s8;
            d8 += 4;
            s8 += 4;
            bytes -= 4;
        }
    }
 
    __asm__ volatile ("sfence" ::: "memory");
}

__attribute__((target("avx")))
static void vga_flip_dirty_avx_impl(uint8_t* dst_base, uint32_t dst_pitch, int x1, int x2, int y1, int y2) {
     int width_pixels = x2 - x1;
     if (width_pixels <= 0) return;
 
     size_t row_bytes = (size_t)width_pixels * 4u;
 
     for (int y = y1; y < y2; y++) {
         const uint8_t* s8 = (const uint8_t*)&back_buffer[y * fb_width + x1];
         uint8_t* d8 = dst_base + (size_t)y * (size_t)dst_pitch + (size_t)x1 * 4u;
         size_t bytes = row_bytes;
 
         while (bytes && (((uintptr_t)d8 & 0x1Fu) != 0u)) {
             *(uint32_t*)d8 = *(const uint32_t*)s8;
             d8 += 4;
             s8 += 4;
             bytes -= 4;
         }
 
         size_t blocks128 = bytes >> 7;
         if (blocks128) {
             if ((((uintptr_t)s8 & 0x1Fu) == 0u)) {
                 __asm__ volatile (
                     "test %2, %2\n\t"
                     "jz 2f\n\t"
                     "1:\n\t"
                     "prefetchnta 512(%0)\n\t"
                     "vmovdqa   (%0), %%ymm0\n\t"
                     "vmovdqa  32(%0), %%ymm1\n\t"
                     "vmovdqa  64(%0), %%ymm2\n\t"
                     "vmovdqa  96(%0), %%ymm3\n\t"
                     "vmovntdq %%ymm0,   (%1)\n\t"
                     "vmovntdq %%ymm1,  32(%1)\n\t"
                     "vmovntdq %%ymm2,  64(%1)\n\t"
                     "vmovntdq %%ymm3,  96(%1)\n\t"
                     "add $128, %0\n\t"
                     "add $128, %1\n\t"
                     "dec %2\n\t"
                     "jnz 1b\n\t"
                     "2:\n\t"
                     : "+r"(s8), "+r"(d8), "+r"(blocks128)
                     :
                     : "memory", "ymm0", "ymm1", "ymm2", "ymm3", "cc"
                 );
             } else {
                 __asm__ volatile (
                     "test %2, %2\n\t"
                     "jz 2f\n\t"
                     "1:\n\t"
                     "prefetchnta 512(%0)\n\t"
                     "vmovdqu   (%0), %%ymm0\n\t"
                     "vmovdqu  32(%0), %%ymm1\n\t"
                     "vmovdqu  64(%0), %%ymm2\n\t"
                     "vmovdqu  96(%0), %%ymm3\n\t"
                     "vmovntdq %%ymm0,   (%1)\n\t"
                     "vmovntdq %%ymm1,  32(%1)\n\t"
                     "vmovntdq %%ymm2,  64(%1)\n\t"
                     "vmovntdq %%ymm3,  96(%1)\n\t"
                     "add $128, %0\n\t"
                     "add $128, %1\n\t"
                     "dec %2\n\t"
                     "jnz 1b\n\t"
                     "2:\n\t"
                     : "+r"(s8), "+r"(d8), "+r"(blocks128)
                     :
                     : "memory", "ymm0", "ymm1", "ymm2", "ymm3", "cc"
                 );
             }
         }
 
         bytes &= 127u;
         size_t blocks32 = bytes >> 5;
         if (blocks32) {
             if ((((uintptr_t)s8 & 0x1Fu) == 0u)) {
                 __asm__ volatile (
                     "test %2, %2\n\t"
                     "jz 2f\n\t"
                     "1:\n\t"
                     "vmovdqa (%0), %%ymm0\n\t"
                     "vmovntdq %%ymm0, (%1)\n\t"
                     "add $32, %0\n\t"
                     "add $32, %1\n\t"
                     "dec %2\n\t"
                     "jnz 1b\n\t"
                     "2:\n\t"
                     : "+r"(s8), "+r"(d8), "+r"(blocks32)
                     :
                     : "memory", "ymm0", "cc"
                 );
             } else {
                 __asm__ volatile (
                     "test %2, %2\n\t"
                     "jz 2f\n\t"
                     "1:\n\t"
                     "vmovdqu (%0), %%ymm0\n\t"
                     "vmovntdq %%ymm0, (%1)\n\t"
                     "add $32, %0\n\t"
                     "add $32, %1\n\t"
                     "dec %2\n\t"
                     "jnz 1b\n\t"
                     "2:\n\t"
                     : "+r"(s8), "+r"(d8), "+r"(blocks32)
                     :
                     : "memory", "ymm0", "cc"
                 );
             }
         }
 
         bytes &= 31u;
         size_t blocks16 = bytes >> 4;
         if (blocks16) {
             __asm__ volatile (
                 "test %2, %2\n\t"
                 "jz 2f\n\t"
                 "1:\n\t"
                 "vmovdqu (%0), %%xmm0\n\t"
                 "vmovntdq %%xmm0, (%1)\n\t"
                 "add $16, %0\n\t"
                 "add $16, %1\n\t"
                 "dec %2\n\t"
                 "jnz 1b\n\t"
                 "2:\n\t"
                 : "+r"(s8), "+r"(d8), "+r"(blocks16)
                 :
                 : "memory", "xmm0", "cc"
             );
         }
 
         bytes &= 15u;
         while (bytes) {
             *(uint32_t*)d8 = *(const uint32_t*)s8;
             d8 += 4;
             s8 += 4;
             bytes -= 4;
         }
     }
 
     __asm__ volatile ("sfence" ::: "memory");
     __asm__ volatile ("vzeroupper" ::: "memory");
 }
 
__attribute__((target("sse2")))
void vga_flip_dirty() {
    if (!fb_kernel_can_render()) return;

    if (dirty_x2 <= dirty_x1 || dirty_y2 <= dirty_y1) return;

    uint32_t* dst_fb = 0;
    uint32_t dst_pitch = 0;
    uint32_t dst_w = 0;
    uint32_t dst_h = 0;
    if (!vga_get_hw_fb(&dst_fb, &dst_pitch, &dst_w, &dst_h)) return;
    if (!back_buffer || !dst_fb || dst_pitch == 0 || dst_w == 0 || dst_h == 0) return;

    int max_w = (int)fb_width;
    int max_h = (int)fb_height;
    if (max_w > (int)dst_w) max_w = (int)dst_w;
    if (max_h > (int)dst_h) max_h = (int)dst_h;
    if (max_w <= 0 || max_h <= 0) return;
 
    int x1 = dirty_x1 & ~3;
    int x2 = (dirty_x2 + 3) & ~3;
    if (x2 > max_w) x2 = max_w;
 
    int y1 = dirty_y1;
    int y2 = dirty_y2;
    if (y1 < 0) y1 = 0;
    if (y2 > max_h) y2 = max_h;
    int width_pixels = x2 - x1;
 
    if (width_pixels <= 0) return;

    uint8_t* dst_base = (uint8_t*)dst_fb;
 
    if (vga_can_use_avx()) {
        vga_flip_dirty_avx_impl(dst_base, dst_pitch, x1, x2, y1, y2);
    } else {
        vga_flip_dirty_sse2_impl(dst_base, dst_pitch, x1, x2, y1, y2);
    }

    if (virtio_gpu_is_active()) {
        (void)virtio_gpu_flush_rect(x1, y1, x2 - x1, y2 - y1);
    }
}

__attribute__((target("sse2"))) __attribute__((always_inline))
static inline void vga_present_row_sse2(uint8_t* d8, const uint8_t* s8, size_t bytes) {
    while (bytes && (((uintptr_t)d8 & 0xFu) != 0u)) {
        *(uint32_t*)d8 = *(const uint32_t*)s8;
        d8 += 4;
        s8 += 4;
        bytes -= 4;
    }

    size_t blocks16 = bytes >> 4;
    if (blocks16) {
        __asm__ volatile(
            "test %2, %2\n\t"
            "jz 2f\n\t"
            "1:\n\t"
            "movdqu (%0), %%xmm0\n\t"
            "movntdq %%xmm0, (%1)\n\t"
            "add $16, %0\n\t"
            "add $16, %1\n\t"
            "dec %2\n\t"
            "jnz 1b\n\t"
            "2:\n\t"
            : "+r"(s8), "+r"(d8), "+r"(blocks16)
            :
            : "memory", "xmm0", "cc"
        );
    }

    bytes &= 15u;
    while (bytes) {
        *(uint32_t*)d8 = *(const uint32_t*)s8;
        d8 += 4;
        s8 += 4;
        bytes -= 4;
    }
}

__attribute__((target("avx"))) __attribute__((always_inline))
static inline void vga_present_row_avx(uint8_t* d8, const uint8_t* s8, size_t bytes) {
    while (bytes && (((uintptr_t)d8 & 0x1Fu) != 0u)) {
        *(uint32_t*)d8 = *(const uint32_t*)s8;
        d8 += 4;
        s8 += 4;
        bytes -= 4;
    }

    size_t blocks32 = bytes >> 5;
    if (blocks32) {
        __asm__ volatile(
            "test %2, %2\n\t"
            "jz 2f\n\t"
            "1:\n\t"
            "vmovdqu (%0), %%ymm0\n\t"
            "vmovntdq %%ymm0, (%1)\n\t"
            "add $32, %0\n\t"
            "add $32, %1\n\t"
            "dec %2\n\t"
            "jnz 1b\n\t"
            "2:\n\t"
            : "+r"(s8), "+r"(d8), "+r"(blocks32)
            :
            : "memory", "ymm0", "cc"
        );
    }

    bytes &= 31u;
    size_t blocks16 = bytes >> 4;
    if (blocks16) {
        __asm__ volatile(
            "test %2, %2\n\t"
            "jz 2f\n\t"
            "1:\n\t"
            "vmovdqu (%0), %%xmm0\n\t"
            "vmovntdq %%xmm0, (%1)\n\t"
            "add $16, %0\n\t"
            "add $16, %1\n\t"
            "dec %2\n\t"
            "jnz 1b\n\t"
            "2:\n\t"
            : "+r"(s8), "+r"(d8), "+r"(blocks16)
            :
            : "memory", "xmm0", "cc"
        );
    }

    bytes &= 15u;
    while (bytes) {
        *(uint32_t*)d8 = *(const uint32_t*)s8;
        d8 += 4;
        s8 += 4;
        bytes -= 4;
    }
}

__attribute__((target("sse2")))
static void vga_present_rect_sse2_impl(uint8_t* dst_base, uint32_t dst_pitch, const uint8_t* src_base, uint32_t src_stride,
                                       int x1, int x2, int y1, int y2) {
     int width_pixels = x2 - x1;
     if (width_pixels <= 0) return;
 
     size_t row_bytes = (size_t)width_pixels * 4u;
     for (int y = y1; y < y2; y++) {
         const uint8_t* s8 = src_base + (size_t)y * (size_t)src_stride + (size_t)x1 * 4u;
         uint8_t* d8 = dst_base + (size_t)y * (size_t)dst_pitch + (size_t)x1 * 4u;
         vga_present_row_sse2(d8, s8, row_bytes);
     }
 
     __asm__ volatile("sfence" ::: "memory");
}
 
__attribute__((target("avx")))
static void vga_present_rect_avx_impl(uint8_t* dst_base, uint32_t dst_pitch, const uint8_t* src_base, uint32_t src_stride,
                                      int x1, int x2, int y1, int y2) {
     int width_pixels = x2 - x1;
     if (width_pixels <= 0) return;
 
     size_t row_bytes = (size_t)width_pixels * 4u;
     for (int y = y1; y < y2; y++) {
         const uint8_t* s8 = src_base + (size_t)y * (size_t)src_stride + (size_t)x1 * 4u;
         uint8_t* d8 = dst_base + (size_t)y * (size_t)dst_pitch + (size_t)x1 * 4u;
         vga_present_row_avx(d8, s8, row_bytes);
     }
 
     __asm__ volatile("sfence" ::: "memory");
     __asm__ volatile("vzeroupper" ::: "memory");
}
 
void vga_present_rect(const void* src, uint32_t src_stride, int x, int y, int w, int h) {
     if (!src) return;
     if (w <= 0 || h <= 0) return;

     if (!fb_kernel_can_render()) return;

     uint32_t* dst_fb = 0;
     uint32_t dst_pitch = 0;
     uint32_t dst_w = 0;
     uint32_t dst_h = 0;
     if (!vga_get_hw_fb(&dst_fb, &dst_pitch, &dst_w, &dst_h)) return;
     if (!dst_fb || dst_pitch == 0 || dst_w == 0 || dst_h == 0) return;

     int max_w = (int)fb_width;
     int max_h = (int)fb_height;
     if (max_w > (int)dst_w) max_w = (int)dst_w;
     if (max_h > (int)dst_h) max_h = (int)dst_h;
     if (max_w <= 0 || max_h <= 0) return;
 
     int x1 = x;
     int y1 = y;
     int x2 = x + w;
     int y2 = y + h;

     if (x1 < 0) x1 = 0;
     if (y1 < 0) y1 = 0;
     if (x2 > max_w) x2 = max_w;
     if (y2 > max_h) y2 = max_h;
     if (x1 >= x2 || y1 >= y2) return;

     x1 &= ~3;
     x2 = (x2 + 3) & ~3;
     if (x2 > max_w) x2 = max_w;
     if (x1 >= x2) return;
 
     const uint8_t* src_base = (const uint8_t*)src;
     uint8_t* dst_base = (uint8_t*)dst_fb;
     if (vga_can_use_avx()) {
         vga_present_rect_avx_impl(dst_base, dst_pitch, src_base, src_stride, x1, x2, y1, y2);
     } else {
         vga_present_rect_sse2_impl(dst_base, dst_pitch, src_base, src_stride, x1, x2, y1, y2);
     }

     if (virtio_gpu_is_active()) {
         (void)virtio_gpu_flush_rect(x1, y1, x2 - x1, y2 - y1);
     }
}
