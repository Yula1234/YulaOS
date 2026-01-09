// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include <lib/string.h>
#include <mm/heap.h>

#include "font8x16.h"

#include "vga.h"

extern uint32_t* fb_ptr;
extern uint32_t  fb_width;
extern uint32_t  fb_height;
extern uint32_t  fb_pitch;

uint32_t* back_buffer;

static uint32_t cursor_x = 0;
static uint32_t cursor_y = 0;
static uint32_t fg_color = COLOR_WHITE;
static uint32_t bg_color = 0x000000;

static uint32_t* vga_current_target = 0;
static uint32_t  vga_target_w = 1024;
static uint32_t  vga_target_h = 768;

int dirty_x1, dirty_y1, dirty_x2, dirty_y2;

void vga_reset_dirty() {
    dirty_x1 = 2000;
    dirty_y1 = 2000;
    dirty_x2 = -2000; 
    dirty_y2 = -2000;
}

void vga_mark_dirty(int x, int y, int w, int h) {
    if (vga_current_target != back_buffer) return;

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

void term_init(term_instance_t* term) {
    if (!term) return;

    spinlock_init(&term->lock);

    term->history_cap_rows = 0;
    term->history_rows = 1;

    if (term->curr_fg == 0) term->curr_fg = COLOR_WHITE;
    if (term->curr_bg == 0) term->curr_bg = COLOR_BLACK;

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

    if (c == '\n') {
        if (term_ensure_rows(term, term->row + 1) != 0) return;

        int idx = term->row * cols + term->col;
        int remaining = cols - term->col;
        for (int k = 0; k < remaining; k++) {
            term->bg_colors[idx + k] = term->curr_bg;
            term->fg_colors[idx + k] = term->curr_fg;
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
        term->fg_colors[idx] = term->curr_fg;
        term->bg_colors[idx] = term->curr_bg;
    } else {
        if (term_ensure_rows(term, term->row + 1) != 0) return;
        int idx = term->row * cols + term->col;
        term->buffer[idx] = c;
        term->fg_colors[idx] = term->curr_fg;
        term->bg_colors[idx] = term->curr_bg;
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

void term_print(term_instance_t* term, const char* s) {
    while (*s) term_putc(term, *s++);
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
                vga_draw_char_sse(c, win_x + x * 8, win_y + y * 8, 0x1E1E1E);
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

__attribute__((target("sse2")))
void vga_draw_rect(int x, int y, int w, int h, uint32_t color) {
    if (!vga_current_target) return;
    if (x >= (int)vga_target_w || y >= (int)vga_target_h) return;
    if (x + w < 0 || y + h < 0) return;

    int x1 = (x < 0) ? 0 : x;
    int y1 = (y < 0) ? 0 : y;
    int x2 = (x + w > (int)vga_target_w) ? (int)vga_target_w : x + w;
    int y2 = (y + h > (int)vga_target_h) ? (int)vga_target_h : y + h;

    int width_to_draw = x2 - x1;
    if (width_to_draw <= 0) return;

    uint32_t sse_width = width_to_draw / 4;
    uint32_t remainder = width_to_draw % 4;

    __asm__ volatile (
        "movd %0, %%xmm0\n\t"
        "pshufd $0, %%xmm0, %%xmm0\n\t"
        : : "r"(color) : "xmm0"
    );

    uint32_t* dest_row = &vga_current_target[y1 * vga_target_w + x1];

    for (int cy = y1; cy < y2; cy++) {
        uint32_t* dest = dest_row;
        
        if (sse_width > 0) {
            uint32_t tmp_count = sse_width;
            uint32_t* tmp_dest = dest;
            __asm__ volatile (
                "1:\n\t"
                "movups %%xmm0, (%0)\n\t"
                "add $16, %0\n\t"
                "dec %1\n\t"
                "jnz 1b\n\t"
                : "+r"(tmp_dest), "+r"(tmp_count)
                :
                : "memory"
            );
        }
        
        for (uint32_t j = 0; j < remainder; j++) {
            dest[j] = color;
        }
        
        dest_row += vga_target_w;
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

    uint32_t* src_ptr = data + skip_x;
    uint32_t* dst_ptr = &vga_current_target[y * vga_target_w + x];

    int screen_stride = vga_target_w * 4;
    int sprite_stride = w * 4;
    int draw_bytes = draw_w * 4;

    __asm__ volatile (
        "movd      %[trans], %%xmm7 \n\t"
        "pshufd    $0x00, %%xmm7, %%xmm7 \n\t"

        "1: \n\t"
        "push      %[w_bytes] \n\t"  
        "push      %[src] \n\t"      
        "push      %[dst] \n\t"      

        "2: \n\t"
        "cmp       $16, %[w_bytes] \n\t"
        "jl        3f \n\t"

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
        "jmp       2b \n\t"

        "3: \n\t"
        "cmp       $0, %[w_bytes] \n\t"
        "jle       4f \n\t"
        
        "mov       (%[src]), %%eax \n\t"
        "cmp       %[trans], %%eax \n\t"
        "je        5f \n\t"
        "mov       %%eax, (%[dst]) \n\t"
        "5: \n\t"
        "add       $4, %[src] \n\t"
        "add       $4, %[dst] \n\t"
        "sub       $4, %[w_bytes] \n\t"
        "jmp       3b \n\t"

        "4: \n\t"
        "pop       %[dst] \n\t"       
        "pop       %[src] \n\t"       
        "pop       %[w_bytes] \n\t"
        
        "add       %[s_stride], %[src] \n\t" 
        "add       %[d_stride], %[dst] \n\t" 
        "dec       %[h] \n\t"
        "jnz       1b \n\t"

        : [src]      "+r" (src_ptr),
          [dst]      "+r" (dst_ptr),
          [h]        "+r" (h),
          [w_bytes]  "+r" (draw_bytes)
        : [trans]    "r"  (trans_color),
          [s_stride] "m"  (sprite_stride),
          [d_stride] "m"  (screen_stride)
        : "memory", "eax", "xmm0", "xmm1", "xmm2", "xmm7"
    );
}

void vga_print_at(const char* s, int x, int y, uint32_t fg) {
    while (*s) {
        vga_draw_char_sse(x + 1, y + 1, *s, 0x000000);
        vga_draw_char_sse(x, y, *s, fg);
        x += 8; 
        s++;
    }
}

__attribute__((target("sse2")))
void vga_flip() {
    uint32_t count = (fb_width * fb_height) / 4; 
    uint32_t* src = back_buffer;
    uint32_t* dst = fb_ptr;

    __asm__ volatile (
        ".loop_flip_fast:\n\t"
        "movaps (%0), %%xmm0\n\t" 
        "movntps %%xmm0, (%1)\n\t"
        "add $16, %0\n\t"
        "add $16, %1\n\t"
        "dec %2\n\t"
        "jnz .loop_flip_fast\n\t"
        "sfence\n\t" 
        : "+r"(src), "+r"(dst), "+r"(count)
        : : "memory", "xmm0", "cc"
    );
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
    
    uint32_t a = alpha;
    uint32_t inv_a = 255 - a;

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

    for (int cy = y1; cy < y2; cy++) {
        uint32_t* dst_ptr = &vga_current_target[cy * vga_target_w + x1];
        
        int count = draw_w / 2; 

        while (count--) {
            __asm__ volatile (
                "movq (%0), %%xmm1\n\t" 
                "movaps %%xmm5, %%xmm0\n\t" 
                "punpcklbw %%xmm4, %%xmm0\n\t"
                "punpcklbw %%xmm4, %%xmm1\n\t" 
                "pmullw %%xmm6, %%xmm0\n\t" 
                "pmullw %%xmm7, %%xmm1\n\t"    
                "paddw %%xmm1, %%xmm0\n\t" 
                "psrlw $8, %%xmm0\n\t"         
                "packuswb %%xmm0, %%xmm0\n\t" 
                "movq %%xmm0, (%0)\n\t" 
                : : "r"(dst_ptr) : "memory", "xmm0", "xmm1"
            );
            dst_ptr += 2;
        }
        
        if (draw_w % 2 != 0) {
            uint32_t bg = *dst_ptr;
            uint32_t rb = ((color & 0xFF00FF) * a + (bg & 0xFF00FF) * inv_a) >> 8;
            uint32_t g  = ((color & 0x00FF00) * a + (bg & 0x00FF00) * inv_a) >> 8;
            *dst_ptr = (rb & 0xFF00FF) | (g & 0x00FF00);
        }
    }
}

__attribute__((target("sse2")))
void vga_draw_gradient_v(int x, int y, int w, int h, uint32_t c1, uint32_t c2) {
    if (!vga_current_target) return;

    if (x >= (int)vga_target_w || y >= (int)vga_target_h) return;
    int x1 = (x < 0) ? 0 : x;
    int y1 = (y < 0) ? 0 : y;
    int x2 = (x + w > (int)vga_target_w) ? (int)vga_target_w : x + w;
    int y2 = (y + h > (int)vga_target_h) ? (int)vga_target_h : y + h;
    int draw_w = x2 - x1;

    if (draw_w <= 0 || y1 >= y2) return;

    for (int cy = y1; cy < y2; cy++) {
        int rel_y = cy - y; 
        
        uint32_t r = (((c1 >> 16) & 0xFF) * (h - rel_y) + ((c2 >> 16) & 0xFF) * rel_y) / h;
        uint32_t g = (((c1 >> 8) & 0xFF) * (h - rel_y) + ((c2 >> 8) & 0xFF) * rel_y) / h;
        uint32_t b = ((c1 & 0xFF) * (h - rel_y) + (c2 & 0xFF) * rel_y) / h;
        uint32_t color = (r << 16) | (g << 8) | b;

        uint32_t* line_ptr = &vga_current_target[cy * vga_target_w + x1];
        
        int count = draw_w;
        
        __asm__ volatile (
            "movd %0, %%xmm0\n\t"
            "pshufd $0, %%xmm0, %%xmm0\n\t"
            : : "r"(color) : "xmm0"
        );

        int sse_blocks = count / 4;
        while (sse_blocks > 0) {
            __asm__ volatile (
                "movups %%xmm0, (%0)\n\t"
                : : "r"(line_ptr) : "memory"
            );
            line_ptr += 4;
            sse_blocks--;
        }
        
        count %= 4;
        while (count--) {
            *line_ptr++ = color;
        }
    }
    
    vga_mark_dirty(x, y, w, h);
}

__attribute__((target("sse2")))
void vga_blur_rect(int x, int y, int w, int h) {
    if (!vga_current_target) return;
    if (x < 1) x = 1;
    if (y < 1) y = 1;
    if (x + w >= (int)vga_target_w) w = vga_target_w - x - 1;
    if (y + h >= (int)vga_target_h) h = vga_target_h - y - 1;
    if (w <= 0 || h <= 0) return;

    for (int i = 0; i < h; i++) {
        int cy = y + i;
        uint32_t* center_ptr = &vga_current_target[cy * vga_target_w + x];
        uint32_t* up_ptr     = &vga_current_target[(cy - 1) * vga_target_w + x];
        uint32_t* down_ptr   = &vga_current_target[(cy + 1) * vga_target_w + x];
        
        int j = 0;
        
        for (; j <= w - 4; j += 4) {
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
    }
    
    vga_mark_dirty(x, y, w, h);
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

    for (int i = 0; i < draw_h; i++) {
        int screen_y = y + i;
        int canvas_y = src_y + i;
        
        uint32_t* dst = &back_buffer[screen_y * fb_width + x];
        uint32_t* src = &canvas[canvas_y * w + src_x];
        
        int count = draw_w / 4; 

        if (count > 0) {
            __asm__ volatile (
                "test %2, %2\n\t"
                "jz .end_blit_sse_safe\n\t"
                ".loop_blit_sse_safe:\n\t"
                "movups (%0), %%xmm0\n\t" 
                "movups %%xmm0, (%1)\n\t" 
                "add $16, %0\n\t"
                "add $16, %1\n\t"
                "dec %2\n\t"
                "jnz .loop_blit_sse_safe\n\t"
                ".end_blit_sse_safe:\n\t"
                : "+r"(src), "+r"(dst), "+r"(count)
                : 
                : "memory", "xmm0", "cc"
            );
        }

        int remainder = draw_w % 4;
        for (int j = 0; j < remainder; j++) {
            dst[j] = src[j];
        }
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

__attribute__((target("sse2")))
void vga_flip_dirty() {
    if (dirty_x2 <= dirty_x1 || dirty_y2 <= dirty_y1) return;

    int x1 = dirty_x1 & ~3;
    int x2 = (dirty_x2 + 3) & ~3;
    if (x2 > (int)fb_width) x2 = fb_width;

    int y1 = dirty_y1;
    int y2 = dirty_y2;
    int width_pixels = x2 - x1;
    int count = width_pixels / 4; 

    if (count <= 0) return;

    for (int y = y1; y < y2; y++) {
        uint32_t* src = &back_buffer[y * fb_width + x1];
        uint32_t* dst = (uint32_t*)((uint8_t*)fb_ptr + y * fb_pitch + x1 * 4);

        if (((uint32_t)dst & 15) == 0) {
            __asm__ volatile (
                "test %%ecx, %%ecx\n\t"
                "jz 2f\n\t"
                "1:\n\t"
                "movdqu (%%esi), %%xmm0\n\t" 
                "movntdq %%xmm0, (%%edi)\n\t"
                "add $16, %%esi\n\t"
                "add $16, %%edi\n\t"
                "dec %%ecx\n\t"
                "jnz 1b\n\t"
                "2:\n\t"
                : "+S"(src), "+D"(dst), "+c"(count)
                : 
                : "memory", "xmm0", "cc"
            );
        } else {
            __asm__ volatile (
                "test %%ecx, %%ecx\n\t"
                "jz 4f\n\t"
                "3:\n\t"
                "movdqu (%%esi), %%xmm0\n\t"
                "movdqu %%xmm0, (%%edi)\n\t"
                "add $16, %%esi\n\t"
                "add $16, %%edi\n\t"
                "dec %%ecx\n\t"
                "jnz 3b\n\t"
                "4:\n\t"
                : "+S"(src), "+D"(dst), "+c"(count)
                : 
                : "memory", "xmm0", "cc"
            );
        }
        
        count = width_pixels / 4;
    }

    __asm__ volatile ("sfence" ::: "memory");
}