// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include <lib/string.h>
#include <mm/heap.h>
#include <hal/simd.h>
#include <drivers/fbdev.h>
#include <drivers/virtio_gpu.h>

#include "font8x16.h"

#include "vga.h"

extern uint32_t* fb_ptr;
extern uint32_t  fb_width;
extern uint32_t  fb_height;
extern uint32_t  fb_pitch;

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

static uint32_t cursor_x = 0;
static uint32_t cursor_y = 0;
static uint32_t fg_color = COLOR_WHITE;
static uint32_t bg_color = 0x000000;

static uint32_t* vga_current_target = 0;
static uint32_t  vga_target_w = 1024;
static uint32_t  vga_target_h = 768;

int dirty_x1, dirty_y1, dirty_x2, dirty_y2;

static inline int vga_can_use_avx(void);
static inline int vga_can_use_avx2(void);

static const uint32_t term_ansi_colors[8] = {
    0x000000u,
    0xAA0000u,
    0x00AA00u,
    0xAA5500u,
    0x0000AAu,
    0xAA00AAu,
    0x00AAAAu,
    0xAAAAAAu
};

static const uint32_t term_ansi_bright_colors[8] = {
    0x555555u,
    0xFF5555u,
    0x55FF55u,
    0xFFFF55u,
    0x5555FFu,
    0xFF55FFu,
    0x55FFFFu,
    0xFFFFFFu
};

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

__attribute__((unused)) static void scroll() {
    uint32_t row_size = fb_pitch * 8;
    uint32_t total_size = fb_pitch * fb_height;
  
    memcpy(fb_ptr, (uint8_t*)fb_ptr + row_size, total_size - row_size);
    
    for (uint32_t y = fb_height - 8; y < fb_height; y++) {
        for (uint32_t x = 0; x < fb_width; x++) {
            vga_put_pixel(x, y, bg_color);
        }
    }
    cursor_y -= 8;
}

char term_buffer[TERM_W * TERM_H];
int term_col = 0, term_row = 0;

void vga_init() {
    memset(term_buffer, ' ', sizeof(term_buffer));
    cursor_x = 0;
    cursor_y = 0;
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

static int term_ensure_rows(term_instance_t* term, int rows_needed) {
    if (!term) return -1;
    if (rows_needed <= 0) rows_needed = 1;
    if (term->history_cap_rows >= rows_needed) return 0;

    int cols = term->cols;
    if (cols <= 0) cols = TERM_W;

    int old_cap = term->history_cap_rows;
    int new_cap = (old_cap > 0) ? old_cap : 128;
    while (new_cap < rows_needed) {
        if (new_cap > (1 << 28)) return -1;
        new_cap *= 2;
    }

    size_t old_cells = (size_t)old_cap * (size_t)cols;
    size_t new_cells = (size_t)new_cap * (size_t)cols;

    term->buffer = (char*)krealloc(term->buffer, new_cells);
    term->fg_colors = (uint32_t*)krealloc(term->fg_colors, new_cells * sizeof(uint32_t));
    term->bg_colors = (uint32_t*)krealloc(term->bg_colors, new_cells * sizeof(uint32_t));

    if (!term->buffer || !term->fg_colors || !term->bg_colors) return -1;

    for (size_t i = old_cells; i < new_cells; i++) {
        term->buffer[i] = ' ';
        term->fg_colors[i] = term->curr_fg;
        term->bg_colors[i] = term->curr_bg;
    }

    term->history_cap_rows = new_cap;
    return 0;
}

static inline uint32_t term_effective_fg(const term_instance_t* term) {
    return (term && term->ansi_inverse) ? term->curr_bg : term->curr_fg;
}

static inline uint32_t term_effective_bg(const term_instance_t* term) {
    return (term && term->ansi_inverse) ? term->curr_fg : term->curr_bg;
}

static void term_ansi_reset(term_instance_t* term) {
    if (!term) return;
    term->esc_state = 0;
    term->csi_in_param = 0;
    term->csi_param_value = 0;
    term->csi_param_count = 0;
}

static void term_csi_push_param(term_instance_t* term) {
    if (!term) return;
    if (term->csi_param_count < (int)(sizeof(term->csi_params) / sizeof(term->csi_params[0]))) {
        int v = term->csi_in_param ? term->csi_param_value : 0;
        term->csi_params[term->csi_param_count++] = v;
    }
    term->csi_param_value = 0;
    term->csi_in_param = 0;
}

static inline int term_csi_param(const term_instance_t* term, int idx, int def) {
    if (!term || idx < 0 || idx >= term->csi_param_count) return def;
    int v = term->csi_params[idx];
    return (v == 0) ? def : v;
}

static void term_set_cursor(term_instance_t* term, int row, int col) {
    if (!term) return;

    int cols = term->cols;
    if (cols <= 0) cols = TERM_W;

    if (row < 0) row = 0;
    if (col < 0) col = 0;
    if (col >= cols) col = cols - 1;

    if (term_ensure_rows(term, row + 1) != 0) return;

    term->row = row;
    term->col = col;

    if (term->row >= term->history_rows) term->history_rows = term->row + 1;
    if (term->row > term->max_row) term->max_row = term->row;
}

static void term_clear_row_range(term_instance_t* term, int row, int x0, int x1) {
    if (!term) return;

    int cols = term->cols;
    if (cols <= 0) cols = TERM_W;

    if (row < 0) return;
    if (x0 < 0) x0 = 0;
    if (x1 > cols) x1 = cols;
    if (x0 >= x1) return;

    if (term_ensure_rows(term, row + 1) != 0) return;

    size_t base = (size_t)row * (size_t)cols + (size_t)x0;
    uint32_t fg = term_effective_fg(term);
    uint32_t bg = term_effective_bg(term);
    for (int x = x0; x < x1; x++) {
        term->buffer[base] = ' ';
        term->fg_colors[base] = fg;
        term->bg_colors[base] = bg;
        base++;
    }

    if (row >= term->history_rows) term->history_rows = row + 1;
    if (row > term->max_row) term->max_row = row;
}

static void term_clear_all(term_instance_t* term) {
    if (!term) return;

    int rows = term->history_rows;
    if (rows < 1) rows = 1;

    for (int r = 0; r < rows; r++) {
        term_clear_row_range(term, r, 0, term->cols);
    }

    term->col = 0;
    term->row = 0;
    term->view_row = 0;
    term->max_row = 0;
    term->history_rows = 1;
}

static void term_apply_sgr(term_instance_t* term) {
    if (!term) return;

    if (term->csi_param_count == 0) {
        term->curr_fg = term->def_fg;
        term->curr_bg = term->def_bg;
        term->ansi_bright = 0;
        term->ansi_inverse = 0;
        return;
    }

    for (int i = 0; i < term->csi_param_count; i++) {
        int p = term->csi_params[i];
        if (p == 0) {
            term->curr_fg = term->def_fg;
            term->curr_bg = term->def_bg;
            term->ansi_bright = 0;
            term->ansi_inverse = 0;
        } else if (p == 1) {
            term->ansi_bright = 1;
        } else if (p == 22) {
            term->ansi_bright = 0;
        } else if (p == 7) {
            term->ansi_inverse = 1;
        } else if (p == 27) {
            term->ansi_inverse = 0;
        } else if (p == 39) {
            term->curr_fg = term->def_fg;
        } else if (p == 49) {
            term->curr_bg = term->def_bg;
        } else if (p >= 30 && p <= 37) {
            int idx = p - 30;
            term->curr_fg = term->ansi_bright ? term_ansi_bright_colors[idx] : term_ansi_colors[idx];
        } else if (p >= 90 && p <= 97) {
            int idx = p - 90;
            term->curr_fg = term_ansi_bright_colors[idx];
        } else if (p >= 40 && p <= 47) {
            int idx = p - 40;
            term->curr_bg = term->ansi_bright ? term_ansi_bright_colors[idx] : term_ansi_colors[idx];
        } else if (p >= 100 && p <= 107) {
            int idx = p - 100;
            term->curr_bg = term_ansi_bright_colors[idx];
        }
    }
}

static void term_handle_csi(term_instance_t* term, char cmd) {
    if (!term) return;

    if (cmd == 'A') {
        int n = term_csi_param(term, 0, 1);
        term_set_cursor(term, term->row - n, term->col);
    } else if (cmd == 'B') {
        int n = term_csi_param(term, 0, 1);
        term_set_cursor(term, term->row + n, term->col);
    } else if (cmd == 'C') {
        int n = term_csi_param(term, 0, 1);
        term_set_cursor(term, term->row, term->col + n);
    } else if (cmd == 'D') {
        int n = term_csi_param(term, 0, 1);
        term_set_cursor(term, term->row, term->col - n);
    } else if (cmd == 'H' || cmd == 'f') {
        int r = term_csi_param(term, 0, 1) - 1;
        int c = term_csi_param(term, 1, 1) - 1;
        term_set_cursor(term, r, c);
    } else if (cmd == 'J') {
        int mode = term->csi_param_count > 0 ? term->csi_params[0] : 0;
        if (mode == 2) {
            term_clear_all(term);
        } else if (mode == 0) {
            term_clear_row_range(term, term->row, term->col, term->cols);
            for (int r = term->row + 1; r < term->view_row + term->view_rows; r++) {
                term_clear_row_range(term, r, 0, term->cols);
            }
        } else if (mode == 1) {
            for (int r = term->view_row; r < term->row; r++) {
                term_clear_row_range(term, r, 0, term->cols);
            }
            term_clear_row_range(term, term->row, 0, term->col + 1);
        }
    } else if (cmd == 'K') {
        int mode = term->csi_param_count > 0 ? term->csi_params[0] : 0;
        if (mode == 0) {
            term_clear_row_range(term, term->row, term->col, term->cols);
        } else if (mode == 1) {
            term_clear_row_range(term, term->row, 0, term->col + 1);
        } else if (mode == 2) {
            term_clear_row_range(term, term->row, 0, term->cols);
        }
    } else if (cmd == 'm') {
        term_apply_sgr(term);
    } else if (cmd == 's') {
        term->saved_row = term->row;
        term->saved_col = term->col;
    } else if (cmd == 'u') {
        term_set_cursor(term, term->saved_row, term->saved_col);
    }
}

void term_init(term_instance_t* term) {
    if (!term) return;

    spinlock_init(&term->lock);

    term->history_cap_rows = 0;
    term->history_rows = 1;

    if (term->curr_fg == 0) term->curr_fg = COLOR_WHITE;
    if (term->curr_bg == 0) term->curr_bg = COLOR_BLACK;
    term->def_fg = term->curr_fg;
    term->def_bg = term->curr_bg;

    if (term->cols <= 0) term->cols = TERM_W;
    if (term->view_rows <= 0) term->view_rows = TERM_H;

    term->buffer = 0;
    term->fg_colors = 0;
    term->bg_colors = 0;

    term_ensure_rows(term, 1);

    term->col = 0;
    term->row = 0;
    term->view_row = 0;
    term->max_row = 0;
    term->saved_col = 0;
    term->saved_row = 0;
    term->esc_state = 0;
    term->csi_in_param = 0;
    term->csi_param_value = 0;
    term->csi_param_count = 0;
    term->ansi_bright = 0;
    term->ansi_inverse = 0;
}

void term_destroy(term_instance_t* term) {
    if (!term) return;
    if (term->buffer) kfree(term->buffer);
    if (term->fg_colors) kfree(term->fg_colors);
    if (term->bg_colors) kfree(term->bg_colors);
    term->buffer = 0;
    term->fg_colors = 0;
    term->bg_colors = 0;
    term->history_cap_rows = 0;
    term->history_rows = 0;
}

void term_clear_row(term_instance_t* term, int row) {
    if (!term || row < 0) return;
    if (term_ensure_rows(term, row + 1) != 0) return;

    int cols = term->cols;
    if (cols <= 0) cols = TERM_W;

    size_t start = (size_t)row * (size_t)cols;
    for (int i = 0; i < cols; i++) {
        term->buffer[start + (size_t)i] = ' ';
        term->fg_colors[start + (size_t)i] = term->curr_fg;
        term->bg_colors[start + (size_t)i] = term->curr_bg;
    }

    if (row >= term->history_rows) term->history_rows = row + 1;
}

void term_get_cell(term_instance_t* term, int row, int col, char* out_ch, uint32_t* out_fg, uint32_t* out_bg) {
    if (out_ch) *out_ch = ' ';
    if (out_fg) *out_fg = term ? term->curr_fg : 0;
    if (out_bg) *out_bg = term ? term->curr_bg : 0;

    int cols = term ? term->cols : 0;
    if (cols <= 0) cols = TERM_W;

    if (!term || row < 0 || col < 0 || col >= cols) return;
    if (row >= term->history_rows) return;

    size_t idx = (size_t)row * (size_t)cols + (size_t)col;
    if (out_ch) *out_ch = term->buffer[idx];
    if (out_fg) *out_fg = term->fg_colors[idx];
    if (out_bg) *out_bg = term->bg_colors[idx];
}

void term_set_cell(term_instance_t* term, int row, int col, char ch, uint32_t fg, uint32_t bg) {
    if (!term) return;

    int cols = term->cols;
    if (cols <= 0) cols = TERM_W;

    if (row < 0 || col < 0 || col >= cols) return;
    if (term_ensure_rows(term, row + 1) != 0) return;

    size_t idx = (size_t)row * (size_t)cols + (size_t)col;
    term->buffer[idx] = ch;
    term->fg_colors[idx] = fg;
    term->bg_colors[idx] = bg;

    if (row >= term->history_rows) term->history_rows = row + 1;
    if (row > term->max_row) term->max_row = row;
}

void term_putc(term_instance_t* term, char c) {
    if (!term) return;

    int cols = term->cols;
    if (cols <= 0) cols = TERM_W;

    int view_rows = term->view_rows;
    if (view_rows <= 0) view_rows = TERM_H;

    if (c == 0x0C) {
        term->col = 0;
        term->row = 0;
        term->view_row = 0;
        term->max_row = 0;
        term->history_rows = 1;
        term_clear_row(term, 0);
        return;
    }

    if (c == '\r') {
        term->col = 0;
        return;
    }

    if (c == '\n') {
        if (term_ensure_rows(term, term->row + 1) != 0) return;

        int idx = term->row * cols + term->col;
        uint32_t fg = term_effective_fg(term);
        uint32_t bg = term_effective_bg(term);
        int remaining = cols - term->col;
        for (int k = 0; k < remaining; k++) {
            term->bg_colors[idx + k] = bg;
            term->fg_colors[idx + k] = fg;
            term->buffer[idx + k] = ' ';
        }

        term->col = 0; 
        term->row++;

        term_clear_row(term, term->row);
    } else if (c == '\b') {
        if (term->col > 0) term->col--;
        if (term_ensure_rows(term, term->row + 1) != 0) return;
        int idx = term->row * cols + term->col;
        term->buffer[idx] = ' ';
        term->fg_colors[idx] = term_effective_fg(term);
        term->bg_colors[idx] = term_effective_bg(term);
    } else {
        if (term_ensure_rows(term, term->row + 1) != 0) return;
        int idx = term->row * cols + term->col;
        term->buffer[idx] = c;
        term->fg_colors[idx] = term_effective_fg(term);
        term->bg_colors[idx] = term_effective_bg(term);
        term->col++;
    }

    if (term->col >= cols) { 
        term->col = 0; 
        term->row++; 
        term_clear_row(term, term->row);
    }

    if (term->row >= term->history_rows) term->history_rows = term->row + 1;
    if (term->row > term->max_row) term->max_row = term->row;

    int at_bottom = (term->view_row + view_rows) >= term->row;
    if (at_bottom) {
        if (term->row >= view_rows) term->view_row = term->row - view_rows + 1;
        else term->view_row = 0;
    }
}

void term_write(term_instance_t* term, const char* buf, uint32_t len) {
    if (!term || !buf || len == 0) return;

    uint32_t i = 0;
    while (i < len) {
        char c = buf[i++];

        if (term->esc_state == 0) {
            if ((uint8_t)c == 0x1Bu) {
                term->esc_state = 1;
                continue;
            }
            if (c == '\r' || c == '\n' || c == '\b' || c == 0x0C) {
                term_putc(term, c);
                continue;
            }
            term_putc(term, c);
            continue;
        }

        if (term->esc_state == 1) {
            if (c == '[') {
                term->esc_state = 2;
                term->csi_param_count = 0;
                term->csi_param_value = 0;
                term->csi_in_param = 0;
                continue;
            }
            if (c == '7') {
                term->saved_row = term->row;
                term->saved_col = term->col;
                term_ansi_reset(term);
                continue;
            }
            if (c == '8') {
                term_set_cursor(term, term->saved_row, term->saved_col);
                term_ansi_reset(term);
                continue;
            }
            term_ansi_reset(term);
            continue;
        }

        if (term->esc_state == 2) {
            if (c >= '0' && c <= '9') {
                term->csi_in_param = 1;
                term->csi_param_value = term->csi_param_value * 10 + (int)(c - '0');
                if (term->csi_param_value > 9999) term->csi_param_value = 9999;
                continue;
            }
            if (c == ';') {
                term_csi_push_param(term);
                continue;
            }

            if (term->csi_in_param || term->csi_param_count > 0) {
                term_csi_push_param(term);
            }
            term_handle_csi(term, c);
            term_ansi_reset(term);
        }
    }
}

void term_print(term_instance_t* term, const char* s) {
    if (!term || !s) return;
    uint32_t len = (uint32_t)strlen(s);
    term_write(term, s, len);
}

void term_reflow(term_instance_t* term, int new_cols) {
    if (!term) return;
    if (new_cols <= 0) new_cols = 1;

    int old_cols = term->cols;
    if (old_cols <= 0) old_cols = TERM_W;
    if (new_cols == old_cols) { term->cols = new_cols; return; }

    int old_last_row = term->max_row;
    if (old_last_row < 0) old_last_row = 0;
    if (old_last_row >= term->history_rows) old_last_row = term->history_rows - 1;
    if (old_last_row < 0) old_last_row = 0;

    size_t worst = ((size_t)(old_last_row + 1) * (size_t)old_cols) + (size_t)(old_last_row + 1);
    int cap_rows = (int)(worst / (size_t)new_cols) + 2;
    if (cap_rows < 1) cap_rows = 1;

    size_t cells = (size_t)cap_rows * (size_t)new_cols;
    char* nb = (char*)kmalloc(cells ? cells : 1);
    uint32_t* nfg = (uint32_t*)kmalloc((cells ? cells : 1) * sizeof(uint32_t));
    uint32_t* nbg = (uint32_t*)kmalloc((cells ? cells : 1) * sizeof(uint32_t));
    if (!nb || !nfg || !nbg) { if (nb) kfree(nb); if (nfg) kfree(nfg); if (nbg) kfree(nbg); return; }

    for (size_t i = 0; i < cells; i++) { nb[i] = ' '; nfg[i] = term->curr_fg; nbg[i] = term->curr_bg; }

    int cur_row = term->row, cur_col = term->col;
    if (cur_row < 0) cur_row = 0;
    if (cur_col < 0) cur_col = 0;
    if (cur_col > old_cols) cur_col = old_cols;

    int out_r = 0, out_c = 0;
    int new_cur_r = 0, new_cur_c = 0, have_cur = 0;
    int new_view_r = 0, have_view = 0;

    for (int r = 0; r <= old_last_row && out_r < cap_rows; r++) {
        if (!have_view && r == term->view_row) { new_view_r = out_r; have_view = 1; }

        int end = old_cols - 1;
        while (end >= 0 && term->buffer[(size_t)r * (size_t)old_cols + (size_t)end] == ' ') end--;
        int row_len = end + 1;
        if (row_len < 0) row_len = 0;

        int take_cur = -1;
        if (r == cur_row) { take_cur = cur_col; if (take_cur > row_len) take_cur = row_len; }

        for (int c = 0; c < row_len && out_r < cap_rows; c++) {
            if (!have_cur && r == cur_row && c == take_cur) { new_cur_r = out_r; new_cur_c = out_c; have_cur = 1; }
            size_t dst = (size_t)out_r * (size_t)new_cols + (size_t)out_c;
            size_t src = (size_t)r * (size_t)old_cols + (size_t)c;
            nb[dst] = term->buffer[src];
            nfg[dst] = term->fg_colors[src];
            nbg[dst] = term->bg_colors[src];
            if (++out_c >= new_cols) { out_c = 0; out_r++; }
        }

        if (!have_cur && r == cur_row && take_cur == row_len) { new_cur_r = out_r; new_cur_c = out_c; have_cur = 1; }

        int hard_nl = (r < old_last_row && end < (old_cols - 1));
        if (hard_nl) { out_r++; out_c = 0; }
    }

    if (out_r >= cap_rows) { out_r = cap_rows - 1; out_c = 0; }

    kfree(term->buffer);
    kfree(term->fg_colors);
    kfree(term->bg_colors);
    term->buffer = nb;
    term->fg_colors = nfg;
    term->bg_colors = nbg;
    term->cols = new_cols;
    term->history_cap_rows = cap_rows;
    term->history_rows = out_r + 1;
    term->max_row = term->history_rows - 1;

    term->view_row = have_view ? new_view_r : term->view_row;
    if (term->view_row < 0) term->view_row = 0;
    if (term->view_row > term->max_row) term->view_row = term->max_row;

    term->row = have_cur ? new_cur_r : out_r;
    term->col = have_cur ? new_cur_c : out_c;
    if (term->row < 0) term->row = 0;
    if (term->row > term->max_row) term->row = term->max_row;
    if (term->col < 0) term->col = 0;
    if (term->col >= term->cols) term->col = term->cols - 1;
}

void vga_render_terminal_instance(term_instance_t* term, int win_x, int win_y) {
    int cols = term ? term->cols : 0;
    if (cols <= 0) cols = TERM_W;
    int view_rows = term ? term->view_rows : 0;
    if (view_rows <= 0) view_rows = TERM_H;

    for (int y = 0; y < view_rows; y++) {
        for (int x = 0; x < cols; x++) {
            char ch;
            uint32_t fg, bg;
            term_get_cell(term, term->view_row + y, x, &ch, &fg, &bg);
            if (bg != COLOR_BLACK) vga_draw_rect(win_x + x * 8, win_y + y * 16, 8, 16, bg);
            if (ch != ' ') vga_draw_char_sse(win_x + x * 8, win_y + y * 16, ch, fg);
        }
    }
}

void vga_putc(char c) {
    if (c == '\n') {
        term_col = 0; term_row++;
    } else if (c == '\b') {
        if (term_col > 0) term_col--;
        term_buffer[term_row * TERM_W + term_col] = ' ';
    } else {
        term_buffer[term_row * TERM_W + term_col] = c;
        term_col++;
    }
    
    if (term_col >= TERM_W) { term_col = 0; term_row++; }
    if (term_row >= TERM_H) {
        memmove(term_buffer, term_buffer + TERM_W, TERM_W * (TERM_H - 1));
        memset(term_buffer + TERM_W * (TERM_H - 1), ' ', TERM_W);
        term_row = TERM_H - 1;
    }
}

void vga_render_terminal(int win_x, int win_y) {
    for (int y = 0; y < TERM_H; y++) {
        for (int x = 0; x < TERM_W; x++) {
            char c = term_buffer[y * TERM_W + x];
            if (c != ' ') {
                vga_draw_char_sse(win_x + x * 8, win_y + y * 16, c, 0x1E1E1E);
            }
        }
    }
}

void vga_clear_terminal() {
    memset(term_buffer, ' ', TERM_W * TERM_H);
    
    term_col = 0;
    term_row = 0;
}

void vga_print(const char* s) {
    while (*s) vga_putc(*s++);
}

void vga_print_u32(uint32_t v) {
    char buf[11];
    int i = 0;
    if (v == 0) { vga_putc('0'); return; }
    while (v) {
        buf[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (i--) vga_putc(buf[i]);
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

__attribute__((target("sse2")))
void vga_draw_sprite_masked(int x, int y, int w, int h, uint32_t* data, uint32_t trans_color) {
    if (!vga_current_target) return;
    
    if (x >= (int)vga_target_w || y >= (int)vga_target_h || x + w <= 0 || y + h <= 0) return;

    if (y < 0) {
        int skip_y = -y;
        if (skip_y >= h) return; 
        h -= skip_y;
        data += skip_y * w;
        y = 0;
    }

    if (y + h > (int)vga_target_h) {
        h = (int)vga_target_h - y;
        if (h <= 0) return;
    }

    int skip_x = 0;
    if (x < 0) {
        skip_x = -x;
        x = 0;
    }

    int draw_w = w - skip_x;
    if (x + draw_w > (int)vga_target_w) {
        draw_w = (int)vga_target_w - x;
    }
    if (draw_w <= 0) return;

    uint8_t* src_ptr = (uint8_t*)(data + skip_x);
    uint8_t* dst_ptr = (uint8_t*)&vga_current_target[y * vga_target_w + x];

    int screen_stride = (int)vga_target_w * 4;
    int sprite_stride = w * 4;
    int draw_bytes = draw_w * 4;

    for (int row = 0; row < h; row++) {
        uint8_t* src_row = src_ptr;
        uint8_t* dst_row = dst_ptr;
        int bytes = draw_bytes;

        __asm__ volatile (
            "movd      %[trans], %%xmm7 \n\t"
            "pshufd    $0x00, %%xmm7, %%xmm7 \n\t"

            "1: \n\t"
            "cmp       $16, %[w_bytes] \n\t"
            "jl        2f \n\t"

            "movdqu    (%[src]), %%xmm0 \n\t"
            "movdqu    (%[dst]), %%xmm1 \n\t"

            "movdqa    %%xmm0, %%xmm2 \n\t"
            "pcmpeqd   %%xmm7, %%xmm2 \n\t"

            "pand      %%xmm2, %%xmm1 \n\t"
            "pandn     %%xmm0, %%xmm2 \n\t"
            "por       %%xmm2, %%xmm1 \n\t"

            "movdqu    %%xmm1, (%[dst]) \n\t"

            "add       $16, %[src] \n\t"
            "add       $16, %[dst] \n\t"
            "sub       $16, %[w_bytes] \n\t"
            "jmp       1b \n\t"

            "2: \n\t"
            "cmp       $0, %[w_bytes] \n\t"
            "jle       3f \n\t"

            "mov       (%[src]), %%eax \n\t"
            "cmp       %[trans], %%eax \n\t"
            "je        4f \n\t"
            "mov       %%eax, (%[dst]) \n\t"
            "4: \n\t"
            "add       $4, %[src] \n\t"
            "add       $4, %[dst] \n\t"
            "sub       $4, %[w_bytes] \n\t"
            "jmp       2b \n\t"

            "3: \n\t"
            : [src] "+r" (src_row),
              [dst] "+r" (dst_row),
              [w_bytes] "+r" (bytes)
            : [trans] "r" (trans_color)
            : "memory", "eax", "xmm0", "xmm1", "xmm2", "xmm7", "cc"
        );

        src_ptr += sprite_stride;
        dst_ptr += screen_stride;
    }
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
 
__attribute__((target("sse2")))
void vga_flip() {
    if (!fb_kernel_can_render()) return;

    vga_present_rect(back_buffer, fb_width * 4u, 0, 0, (int)fb_width, (int)fb_height);
}

void vga_draw_cursor(int x, int y) {
    vga_draw_rect(x, y, 5, 5, 0xFFFFFF);
}

void term_print_u32(term_instance_t* term, uint32_t n) {
    char buf[12];
    int i = 0;
    if (n == 0) { term_putc(term, '0'); return; }
    while (n > 0) { buf[i++] = (n % 10) + '0'; n /= 10; }
    while (i--) term_putc(term, buf[i]);
}

__attribute__((unused)) static uint32_t color_blend(uint32_t c1, uint32_t c2, uint8_t alpha) {
    if (alpha == 255) return c1;
    if (alpha == 0) return c2;

    uint32_t rb = ((c1 & 0xFF00FF) * alpha + (c2 & 0xFF00FF) * (256 - alpha)) >> 8;
    uint32_t g  = ((c1 & 0x00FF00) * alpha + (c2 & 0x00FF00) * (256 - alpha)) >> 8;
    return (rb & 0xFF00FF) | (g & 0x00FF00);
}

__attribute__((target("sse2"))) __attribute__((always_inline))
static inline void vga_draw_rect_alpha_sse2_impl(int x1, int y1, int draw_w, int draw_h, uint32_t color, uint8_t alpha) {
    const int stride = (int)vga_target_w;
    uint32_t* row = vga_current_target + y1 * stride + x1;

    if (alpha == 255) {
        uint32_t blocks4 = (uint32_t)draw_w >> 2;
        uint32_t rem = (uint32_t)draw_w & 3u;

        __asm__ volatile (
            "movd %0, %%xmm0\n\t"
            "pshufd $0, %%xmm0, %%xmm0\n\t"
            : : "r"(color) : "xmm0"
        );

        for (int cy = 0; cy < draw_h; cy++) {
            uint32_t* dst = row;
            uint32_t n = blocks4;

            if (n) {
                __asm__ volatile (
                    "1:\n\t"
                    "movups %%xmm0, (%0)\n\t"
                    "add $16, %0\n\t"
                    "dec %1\n\t"
                    "jnz 1b\n\t"
                    : "+r"(dst), "+r"(n)
                    :
                    : "memory", "cc"
                );
            }

            for (uint32_t i = 0; i < rem; i++) dst[i] = color;
            row += stride;
        }
        return;
    }

    uint32_t a = (uint32_t)alpha;
    uint32_t inv_a = 255u - a;

    __asm__ volatile (
        "movd %0, %%xmm6\n\t"
        "pshuflw $0, %%xmm6, %%xmm6\n\t"
        "punpcklqdq %%xmm6, %%xmm6\n\t"
        "movd %1, %%xmm7\n\t"
        "pshuflw $0, %%xmm7, %%xmm7\n\t"
        "punpcklqdq %%xmm7, %%xmm7\n\t"
        "pxor %%xmm4, %%xmm4\n\t"
        "movd %2, %%xmm5\n\t"
        "pshufd $0, %%xmm5, %%xmm5\n\t"
        : : "r"(a), "r"(inv_a), "r"(color) : "xmm4", "xmm5", "xmm6", "xmm7"
    );

    size_t blocks8 = (size_t)draw_w >> 3;
    size_t blocks4 = ((size_t)draw_w & 7u) >> 2;
    int tail = draw_w & 3;

    for (int cy = 0; cy < draw_h; cy++) {
        uint8_t* p = (uint8_t*)row;

        size_t n8 = blocks8;
        if (n8) {
            __asm__ volatile (
                "1:\n\t"
                "movdqu   (%0), %%xmm1\n\t"
                "movdqa %%xmm1, %%xmm2\n\t"
                "movdqa %%xmm5, %%xmm0\n\t"
                "punpcklbw %%xmm4, %%xmm0\n\t"
                "punpcklbw %%xmm4, %%xmm1\n\t"
                "pmullw %%xmm6, %%xmm0\n\t"
                "pmullw %%xmm7, %%xmm1\n\t"
                "paddw %%xmm1, %%xmm0\n\t"
                "psrlw $8, %%xmm0\n\t"
                "movdqa %%xmm5, %%xmm3\n\t"
                "punpckhbw %%xmm4, %%xmm3\n\t"
                "punpckhbw %%xmm4, %%xmm2\n\t"
                "pmullw %%xmm6, %%xmm3\n\t"
                "pmullw %%xmm7, %%xmm2\n\t"
                "paddw %%xmm2, %%xmm3\n\t"
                "psrlw $8, %%xmm3\n\t"
                "packuswb %%xmm3, %%xmm0\n\t"
                "movdqu %%xmm0,   (%0)\n\t"

                "movdqu 16(%0), %%xmm1\n\t"
                "movdqa %%xmm1, %%xmm2\n\t"
                "movdqa %%xmm5, %%xmm0\n\t"
                "punpcklbw %%xmm4, %%xmm0\n\t"
                "punpcklbw %%xmm4, %%xmm1\n\t"
                "pmullw %%xmm6, %%xmm0\n\t"
                "pmullw %%xmm7, %%xmm1\n\t"
                "paddw %%xmm1, %%xmm0\n\t"
                "psrlw $8, %%xmm0\n\t"
                "movdqa %%xmm5, %%xmm3\n\t"
                "punpckhbw %%xmm4, %%xmm3\n\t"
                "punpckhbw %%xmm4, %%xmm2\n\t"
                "pmullw %%xmm6, %%xmm3\n\t"
                "pmullw %%xmm7, %%xmm2\n\t"
                "paddw %%xmm2, %%xmm3\n\t"
                "psrlw $8, %%xmm3\n\t"
                "packuswb %%xmm3, %%xmm0\n\t"
                "movdqu %%xmm0, 16(%0)\n\t"

                "add $32, %0\n\t"
                "dec %1\n\t"
                "jnz 1b\n\t"
                : "+r"(p), "+r"(n8)
                :
                : "memory", "xmm0", "xmm1", "xmm2", "xmm3", "cc"
            );
        }

        if (blocks4) {
            __asm__ volatile (
                "movdqu   (%0), %%xmm1\n\t"
                "movdqa %%xmm1, %%xmm2\n\t"
                "movdqa %%xmm5, %%xmm0\n\t"
                "punpcklbw %%xmm4, %%xmm0\n\t"
                "punpcklbw %%xmm4, %%xmm1\n\t"
                "pmullw %%xmm6, %%xmm0\n\t"
                "pmullw %%xmm7, %%xmm1\n\t"
                "paddw %%xmm1, %%xmm0\n\t"
                "psrlw $8, %%xmm0\n\t"
                "movdqa %%xmm5, %%xmm3\n\t"
                "punpckhbw %%xmm4, %%xmm3\n\t"
                "punpckhbw %%xmm4, %%xmm2\n\t"
                "pmullw %%xmm6, %%xmm3\n\t"
                "pmullw %%xmm7, %%xmm2\n\t"
                "paddw %%xmm2, %%xmm3\n\t"
                "psrlw $8, %%xmm3\n\t"
                "packuswb %%xmm3, %%xmm0\n\t"
                "movdqu %%xmm0,   (%0)\n\t"
                "add $16, %0\n\t"
                : "+r"(p)
                :
                : "memory", "xmm0", "xmm1", "xmm2", "xmm3"
            );
        }

        uint32_t* tailp = (uint32_t*)p;
        for (int i = 0; i < tail; i++) {
            uint32_t bg = tailp[i];
            uint32_t rb = ((color & 0xFF00FF) * a + (bg & 0xFF00FF) * inv_a) >> 8;
            uint32_t g  = ((color & 0x00FF00) * a + (bg & 0x00FF00) * inv_a) >> 8;
            tailp[i] = (rb & 0xFF00FF) | (g & 0x00FF00);
        }

        row += stride;
    }
}

__attribute__((target("avx2")))
static void vga_draw_rect_alpha_avx2_impl(int x1, int y1, int draw_w, int draw_h, uint32_t color, uint8_t alpha) {
    const int stride = (int)vga_target_w;
    uint32_t* row = vga_current_target + y1 * stride + x1;

    if (alpha == 255) {
        size_t blocks8 = (size_t)draw_w >> 3;
        size_t rem = (size_t)draw_w & 7u;

        __asm__ volatile (
            "vmovd %0, %%xmm0\n\t"
            "vpbroadcastd %%xmm0, %%ymm0\n\t"
            : : "r"(color) : "xmm0", "ymm0"
        );

        for (int cy = 0; cy < draw_h; cy++) {
            uint8_t* p = (uint8_t*)row;
            size_t n8 = blocks8;
            if (n8) {
                __asm__ volatile (
                    "1:\n\t"
                    "vmovdqu %%ymm0, (%0)\n\t"
                    "add $32, %0\n\t"
                    "dec %1\n\t"
                    "jnz 1b\n\t"
                    : "+r"(p), "+r"(n8)
                    :
                    : "memory", "cc"
                );
            }

            uint32_t* t = (uint32_t*)p;
            for (size_t i = 0; i < rem; i++) t[i] = color;
            row += stride;
        }

        __asm__ volatile ("vzeroupper" ::: "memory");
        return;
    }

    uint32_t a = (uint32_t)alpha;
    uint32_t inv_a = 255u - a;
    uint32_t mask_rgb = 0x00FFFFFFu;

    __asm__ volatile (
        "vmovd %0, %%xmm6\n\t"
        "vpbroadcastw %%xmm6, %%ymm6\n\t"
        "vmovd %1, %%xmm7\n\t"
        "vpbroadcastw %%xmm7, %%ymm7\n\t"
        "vpxor %%ymm4, %%ymm4, %%ymm4\n\t"
        "vmovd %2, %%xmm5\n\t"
        "vpbroadcastd %%xmm5, %%ymm5\n\t"
        "vpunpcklbw %%ymm4, %%ymm5, %%ymm0\n\t"
        "vpunpckhbw %%ymm4, %%ymm5, %%ymm1\n\t"
        "vpmullw %%ymm6, %%ymm0, %%ymm0\n\t"
        "vpmullw %%ymm6, %%ymm1, %%ymm1\n\t"

        "vmovd %3, %%xmm3\n\t"
        "vpbroadcastd %%xmm3, %%ymm3\n\t"
        :
        : "r"(a), "r"(inv_a), "r"(color), "r"(mask_rgb)
        : "cc", "ymm0", "ymm1", "ymm3", "ymm4", "ymm5", "ymm6", "ymm7", "xmm3", "xmm5", "xmm6", "xmm7"
    );

    size_t n8_total = (size_t)draw_w >> 3;
    size_t blocks4 = ((size_t)draw_w & 7u) >> 2;
    int tail = draw_w & 3;

    for (int cy = 0; cy < draw_h; cy++) {
        uint8_t* p = (uint8_t*)row;
        size_t pairs = n8_total >> 1;
        if (pairs) {
            __asm__ volatile (
                "1:\n\t"
                "prefetchnta 512(%0)\n\t"

                "vmovdqu   (%0), %%ymm2\n\t"
                "vpunpcklbw %%ymm4, %%ymm2, %%ymm5\n\t"
                "vpunpckhbw %%ymm4, %%ymm2, %%ymm6\n\t"
                "vpmullw %%ymm7, %%ymm5, %%ymm5\n\t"
                "vpmullw %%ymm7, %%ymm6, %%ymm6\n\t"
                "vpaddw %%ymm0, %%ymm5, %%ymm5\n\t"
                "vpaddw %%ymm1, %%ymm6, %%ymm6\n\t"
                "vpsrlw $8, %%ymm5, %%ymm5\n\t"
                "vpsrlw $8, %%ymm6, %%ymm6\n\t"
                "vpackuswb %%ymm6, %%ymm5, %%ymm5\n\t"
                "vpand %%ymm3, %%ymm5, %%ymm5\n\t"
                "vmovdqu %%ymm5,   (%0)\n\t"

                "vmovdqu 32(%0), %%ymm4\n\t"
                "vpunpcklbw %%ymm4, %%ymm4, %%ymm5\n\t"
                "vpunpckhbw %%ymm4, %%ymm4, %%ymm6\n\t"
                "vpmullw %%ymm7, %%ymm5, %%ymm5\n\t"
                "vpmullw %%ymm7, %%ymm6, %%ymm6\n\t"
                "vpaddw %%ymm0, %%ymm5, %%ymm5\n\t"
                "vpaddw %%ymm1, %%ymm6, %%ymm6\n\t"
                "vpsrlw $8, %%ymm5, %%ymm5\n\t"
                "vpsrlw $8, %%ymm6, %%ymm6\n\t"
                "vpackuswb %%ymm6, %%ymm5, %%ymm5\n\t"
                "vpand %%ymm3, %%ymm5, %%ymm5\n\t"
                "vmovdqu %%ymm5,  32(%0)\n\t"

                "add $64, %0\n\t"
                "dec %1\n\t"
                "jnz 1b\n\t"
                : "+r"(p), "+r"(pairs)
                :
                : "memory", "cc", "ymm2", "ymm5", "ymm6"
            );
        }

        if (n8_total & 1u) {
            __asm__ volatile (
                "vmovdqu (%0), %%ymm2\n\t"
                "vpunpcklbw %%ymm4, %%ymm2, %%ymm5\n\t"
                "vpunpckhbw %%ymm4, %%ymm2, %%ymm6\n\t"
                "vpmullw %%ymm7, %%ymm5, %%ymm5\n\t"
                "vpmullw %%ymm7, %%ymm6, %%ymm6\n\t"
                "vpaddw %%ymm0, %%ymm5, %%ymm5\n\t"
                "vpaddw %%ymm1, %%ymm6, %%ymm6\n\t"
                "vpsrlw $8, %%ymm5, %%ymm5\n\t"
                "vpsrlw $8, %%ymm6, %%ymm6\n\t"
                "vpackuswb %%ymm6, %%ymm5, %%ymm5\n\t"
                "vpand %%ymm3, %%ymm5, %%ymm5\n\t"
                "vmovdqu %%ymm5, (%0)\n\t"
                "add $32, %0\n\t"
                : "+r"(p)
                :
                : "memory", "ymm2", "ymm5", "ymm6"
            );
        }

        if (blocks4) {
            __asm__ volatile (
                "vmovdqu   (%0), %%xmm2\n\t"
                "vpunpcklbw %%xmm4, %%xmm2, %%xmm5\n\t"
                "vpunpckhbw %%xmm4, %%xmm2, %%xmm6\n\t"
                "vpmullw %%xmm7, %%xmm5, %%xmm5\n\t"
                "vpmullw %%xmm7, %%xmm6, %%xmm6\n\t"
                "vpaddw %%xmm0, %%xmm5, %%xmm5\n\t"
                "vpaddw %%xmm1, %%xmm6, %%xmm6\n\t"
                "vpsrlw $8, %%xmm5, %%xmm5\n\t"
                "vpsrlw $8, %%xmm6, %%xmm6\n\t"
                "vpackuswb %%xmm6, %%xmm5, %%xmm5\n\t"
                "vpand %%xmm3, %%xmm5, %%xmm5\n\t"
                "vmovdqu %%xmm5,   (%0)\n\t"
                "add $16, %0\n\t"
                : "+r"(p)
                :
                : "memory", "xmm2", "xmm5", "xmm6"
            );
        }

        uint32_t* tailp = (uint32_t*)p;
        for (int i = 0; i < tail; i++) {
            uint32_t bg = tailp[i];
            uint32_t rb = ((color & 0xFF00FF) * a + (bg & 0xFF00FF) * inv_a) >> 8;
            uint32_t g  = ((color & 0x00FF00) * a + (bg & 0x00FF00) * inv_a) >> 8;
            tailp[i] = (rb & 0xFF00FF) | (g & 0x00FF00);
        }

        row += stride;
    }

    __asm__ volatile ("vzeroupper" ::: "memory");
}

__attribute__((target("sse2")))
void vga_draw_rect_alpha(int x, int y, int w, int h, uint32_t color, uint8_t alpha) {
    if (!vga_current_target || alpha == 0) return;

    if (x >= (int)vga_target_w || y >= (int)vga_target_h) return;
    if (x + w <= 0 || y + h <= 0) return;

    int x1 = (x < 0) ? 0 : x;
    int y1 = (y < 0) ? 0 : y;
    int x2 = (x + w > (int)vga_target_w) ? (int)vga_target_w : x + w;
    int y2 = (y + h > (int)vga_target_h) ? (int)vga_target_h : y + h;

    int draw_w = x2 - x1;
    int draw_h = y2 - y1;

    if (draw_w <= 0 || draw_h <= 0) return;

    if (vga_can_use_avx2()) {
        vga_draw_rect_alpha_avx2_impl(x1, y1, draw_w, draw_h, color, alpha);
    } else {
        vga_draw_rect_alpha_sse2_impl(x1, y1, draw_w, draw_h, color, alpha);
    }
}

__attribute__((target("sse2"))) __attribute__((always_inline))
static inline void vga_blur_rect_sse2_impl(int x, int y, int w, int h) {
    const int stride = (int)vga_target_w;
    uint32_t* row = vga_current_target + y * stride + x;

    for (int i = 0; i < h; i++) {
        uint32_t* center_ptr = row;
        uint32_t* up_ptr     = row - stride;
        uint32_t* down_ptr   = row + stride;

        for (int j = 0; j <= w - 4; j += 4) {
            __asm__ volatile (
                "movdqu (%1), %%xmm0 \n\t"
                "movdqu (%2), %%xmm1 \n\t"
                "pavgb %%xmm1, %%xmm0 \n\t"

                "movdqu -4(%0), %%xmm2 \n\t"
                "movdqu 4(%0), %%xmm3 \n\t"
                "pavgb %%xmm3, %%xmm2 \n\t"

                "pavgb %%xmm2, %%xmm0 \n\t"

                "movdqu %%xmm0, (%0) \n\t"
                :
                : "r"(center_ptr), "r"(up_ptr), "r"(down_ptr)
                : "memory", "xmm0", "xmm1", "xmm2", "xmm3"
            );

            center_ptr += 4;
            up_ptr += 4;
            down_ptr += 4;
        }

        row += stride;
    }
}

__attribute__((target("avx2")))
static void vga_blur_rect_avx2_impl(int x, int y, int w, int h) {
    int blocks8 = w >> 3;
    int blocks4 = (w - (blocks8 << 3)) >> 2;

    const int stride = (int)vga_target_w;
    uint32_t* row = vga_current_target + y * stride + x;

    for (int i = 0; i < h; i++) {
        uint8_t* c8 = (uint8_t*)row;
        uint8_t* u8 = (uint8_t*)(row - stride);
        uint8_t* d8 = (uint8_t*)(row + stride);

        size_t n8 = (size_t)blocks8;
        size_t pairs = n8 >> 1;
        if (pairs) {
            __asm__ volatile (
                "1:\n\t"
                "prefetchnta 512(%1)\n\t"
                "prefetchnta 512(%2)\n\t"
                "vmovdqu   (%1), %%ymm0\n\t"
                "vmovdqu   (%2), %%ymm1\n\t"
                "vpavgb  %%ymm1, %%ymm0, %%ymm0\n\t"
                "vmovdqu -4(%0), %%ymm2\n\t"
                "vmovdqu  4(%0), %%ymm3\n\t"
                "vpavgb  %%ymm3, %%ymm2, %%ymm2\n\t"
                "vpavgb  %%ymm2, %%ymm0, %%ymm0\n\t"
                "vmovdqu %%ymm0,   (%0)\n\t"
                "vmovdqu  32(%1), %%ymm4\n\t"
                "vmovdqu  32(%2), %%ymm5\n\t"
                "vpavgb  %%ymm5, %%ymm4, %%ymm4\n\t"
                "vmovdqu 28(%0), %%ymm6\n\t"
                "vmovdqu 36(%0), %%ymm7\n\t"
                "vpavgb  %%ymm7, %%ymm6, %%ymm6\n\t"
                "vpavgb  %%ymm6, %%ymm4, %%ymm4\n\t"
                "vmovdqu %%ymm4,  32(%0)\n\t"
                "add $64, %0\n\t"
                "add $64, %1\n\t"
                "add $64, %2\n\t"
                "dec %3\n\t"
                "jnz 1b\n\t"
                : "+r"(c8), "+r"(u8), "+r"(d8), "+r"(pairs)
                :
                : "memory", "ymm0", "ymm1", "ymm2", "ymm3", "ymm4", "ymm5", "ymm6", "ymm7", "cc"
            );
        }

        if (n8 & 1u) {
            __asm__ volatile (
                "vmovdqu (%1), %%ymm0\n\t"
                "vmovdqu (%2), %%ymm1\n\t"
                "vpavgb  %%ymm1, %%ymm0, %%ymm0\n\t"
                "vmovdqu -4(%0), %%ymm2\n\t"
                "vmovdqu  4(%0), %%ymm3\n\t"
                "vpavgb  %%ymm3, %%ymm2, %%ymm2\n\t"
                "vpavgb  %%ymm2, %%ymm0, %%ymm0\n\t"
                "vmovdqu %%ymm0, (%0)\n\t"
                "add $32, %0\n\t"
                "add $32, %1\n\t"
                "add $32, %2\n\t"
                : "+r"(c8), "+r"(u8), "+r"(d8)
                :
                : "memory", "ymm0", "ymm1", "ymm2", "ymm3"
            );
        }

        uint32_t* center_ptr = (uint32_t*)c8;
        uint32_t* up_ptr     = (uint32_t*)u8;
        uint32_t* down_ptr   = (uint32_t*)d8;

        for (int j = 0; j < blocks4; j++) {
            __asm__ volatile (
                "vmovdqu (%1), %%xmm0 \n\t"
                "vmovdqu (%2), %%xmm1 \n\t"
                "vpavgb %%xmm1, %%xmm0, %%xmm0 \n\t"

                "vmovdqu -4(%0), %%xmm2 \n\t"
                "vmovdqu 4(%0), %%xmm3 \n\t"
                "vpavgb %%xmm3, %%xmm2, %%xmm2 \n\t"

                "vpavgb %%xmm2, %%xmm0, %%xmm0 \n\t"

                "vmovdqu %%xmm0, (%0) \n\t"
                :
                : "r"(center_ptr), "r"(up_ptr), "r"(down_ptr)
                : "memory", "xmm0", "xmm1", "xmm2", "xmm3"
            );

            center_ptr += 4;
            up_ptr += 4;
            down_ptr += 4;
        }

        row += stride;
    }

    __asm__ volatile ("vzeroupper" ::: "memory");
}

__attribute__((target("sse2")))
void vga_blur_rect(int x, int y, int w, int h) {
    if (!vga_current_target) return;
    if (x < 1) x = 1;
    if (y < 1) y = 1;
    if (x + w >= (int)vga_target_w) w = vga_target_w - x - 1;
    if (y + h >= (int)vga_target_h) h = vga_target_h - y - 1;
    if (w <= 0 || h <= 0) return;

    if (vga_can_use_avx2()) {
        vga_blur_rect_avx2_impl(x, y, w, h);
    } else {
        vga_blur_rect_sse2_impl(x, y, w, h);
    }

    vga_mark_dirty(x, y, w, h);
}

__attribute__((target("sse2"))) __attribute__((always_inline))
static inline void vga_blit_canvas_sse2_impl(int x, int y, uint32_t* canvas, int w, int src_x, int src_y, int draw_w, int draw_h) {
    if (!canvas || !back_buffer) return;
    if (w <= 0 || draw_w <= 0 || draw_h <= 0) return;

    const size_t row_bytes = (size_t)draw_w * 4u;

    for (int i = 0; i < draw_h; i++) {
        const int screen_y = y + i;
        const uint32_t* src = &canvas[(src_y + i) * w + src_x];
        uint32_t* dst = &back_buffer[screen_y * fb_width + x];
        memcpy(dst, src, row_bytes);
    }
}

__attribute__((target("avx")))
static void vga_blit_canvas_avx_impl(int x, int y, uint32_t* canvas, int w, int src_x, int src_y, int draw_w, int draw_h) {
    if (!canvas || !back_buffer) return;
    if (w <= 0 || draw_w <= 0 || draw_h <= 0) return;

    for (int i = 0; i < draw_h; i++) {
        int screen_y = y + i;

        uint8_t* d8 = (uint8_t*)&back_buffer[screen_y * fb_width + x];
        const uint8_t* s8 = (const uint8_t*)&canvas[(src_y + i) * w + src_x];

        size_t bytes = (size_t)draw_w * 4u;

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

    __asm__ volatile("sfence" ::: "memory");
    __asm__ volatile("vzeroupper" ::: "memory");
}

__attribute__((target("sse2")))
void vga_blit_canvas(int x, int y, uint32_t* canvas, int w, int h) {
    if (!canvas) return;

    if (x >= (int)fb_width || y >= (int)fb_height) return;
    if (x + w <= 0 || y + h <= 0) return;

    int src_x = 0;
    int src_y = 0;
    int draw_w = w;
    int draw_h = h;

    if (x < 0) {
        src_x = -x;
        draw_w += x;
        x = 0;
    }
    if (y < 0) {
        src_y = -y;
        draw_h += y;
        y = 0;
    }
    if (x + draw_w > (int)fb_width) {
        draw_w = (int)fb_width - x;
    }
    if (y + draw_h > (int)fb_height) {
        draw_h = (int)fb_height - y;
    }

    if (draw_w <= 0 || draw_h <= 0) return;

    if (vga_can_use_avx()) {
        vga_blit_canvas_avx_impl(x, y, canvas, w, src_x, src_y, draw_w, draw_h);
    } else {
        vga_blit_canvas_sse2_impl(x, y, canvas, w, src_x, src_y, draw_w, draw_h);
    }

    vga_mark_dirty(x, y, draw_w, draw_h);
}

__attribute__((target("sse2")))
void vga_draw_sprite_scaled_masked(int x, int y, int sw, int sh, int scale, uint32_t* data, uint32_t trans) {
    if (!vga_current_target) return;

    if (x >= (int)vga_target_w || y >= (int)vga_target_h) return;
    if (x + sw * scale <= 0 || y + sh * scale <= 0) return;

    for (int i = 0; i < sh; i++) {
        int screen_y_start = y + i * scale;
        
        if (screen_y_start >= (int)vga_target_h) break;
        if (screen_y_start + scale <= 0) continue;

        for (int j = 0; j < sw; j++) {
            uint32_t color = data[i * sw + j];
            if (color == trans) continue;

            int screen_x_start = x + j * scale;
            
            if (screen_x_start >= (int)vga_target_w) break;
            if (screen_x_start + scale <= 0) continue;

            __asm__ volatile (
                "movd %0, %%xmm0\n\t"
                "pshufd $0, %%xmm0, %%xmm0\n\t"
                : : "r"(color) : "xmm0"
            );

            for (int sy = 0; sy < scale; sy++) {
                int py = screen_y_start + sy;
                if (py < 0 || py >= (int)vga_target_h) continue;

                uint32_t* dst = &vga_current_target[py * vga_target_w + screen_x_start];
                int width_to_draw = scale;
                
                int start_off = 0;
                if (screen_x_start < 0) {
                    start_off = -screen_x_start;
                    width_to_draw -= start_off;
                    dst += start_off;
                }
                if (screen_x_start + scale > (int)vga_target_w) {
                    width_to_draw = (int)vga_target_w - screen_x_start;
                }

                if (width_to_draw <= 0) continue;

                int k = 0;
                while (width_to_draw - k >= 4) {
                    __asm__ volatile (
                        "movups %%xmm0, (%0)\n\t"
                        : : "r"(dst + k) : "memory"
                    );
                    k += 4;
                }
                while (k < width_to_draw) {
                    dst[k++] = color;
                }
            }
        }
    }
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
