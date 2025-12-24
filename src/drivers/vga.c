#include "vga.h"
#include "../lib/string.h"
#include "font8x8.h"
#include "font8x16.h"

extern uint32_t* fb_ptr;
extern uint32_t  fb_width;
extern uint32_t  fb_height;
extern uint32_t  fb_pitch;

uint32_t* back_buffer;

static uint32_t cursor_x = 0;
static uint32_t cursor_y = 0;
static uint32_t fg_color = COLOR_WHITE;
static uint32_t bg_color = COLOR_BLUE;

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

__attribute__((target("sse2")))
void vga_draw_char_sse(int x, int y, char c, uint32_t fg) {
    if ((uint8_t)c > 127 || !vga_current_target) return;
    
    if (x < 0 || y < 0 || x + 8 > (int)vga_target_w || y + 16 > (int)vga_target_h) return;

    const uint8_t* glyph = font8x16_basic[(uint8_t)c];

    for (int i = 0; i < 16; i++) {
        uint8_t row = glyph[i];
        if (row == 0) continue;

        uint32_t* dst = &vga_current_target[(y + i) * vga_target_w + x];
        
        if (row & 0x80) dst[0] = fg;
        if (row & 0x40) dst[1] = fg;
        if (row & 0x20) dst[2] = fg;
        if (row & 0x10) dst[3] = fg;
        if (row & 0x08) dst[4] = fg;
        if (row & 0x04) dst[5] = fg;
        if (row & 0x02) dst[6] = fg;
        if (row & 0x01) dst[7] = fg;
    }
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

static void term_fill_colors(term_instance_t* term, uint32_t fg, uint32_t bg) {
    for (int i = 0; i < TERM_W * TERM_HISTORY; i++) {
        term->fg_colors[i] = fg;
        term->bg_colors[i] = bg;
    }
}

void term_putc(term_instance_t* term, char c) {
    if (c == 0x0C) { 
        memset(term->buffer, ' ', TERM_W * TERM_HISTORY);
        term_fill_colors(term, term->curr_fg, term->curr_bg);
        
        term->col = 0;
        term->row = 0;
        term->view_row = 0;
        term->max_row = 0;
        return;
    }

    if (c == '\n') {
        int idx = term->row * TERM_W + term->col;
        int remaining = TERM_W - term->col;
        for(int k=0; k<remaining; k++) {
            term->bg_colors[idx+k] = term->curr_bg;
            term->fg_colors[idx+k] = term->curr_fg;
        }

        term->col = 0; 
        term->row++;
    } else if (c == '\b') {
        if (term->col > 0) term->col--;
        int idx = term->row * TERM_W + term->col;
        term->buffer[idx] = ' ';
        term->fg_colors[idx] = term->curr_fg;
        term->bg_colors[idx] = term->curr_bg;
    } else {
        int idx = term->row * TERM_W + term->col;
        term->buffer[idx] = c;
        term->fg_colors[idx] = term->curr_fg;
        term->bg_colors[idx] = term->curr_bg;
        term->col++;
    }

    if (term->col >= TERM_W) { 
        term->col = 0; 
        term->row++; 
    }

    if (term->row >= TERM_HISTORY) {
        memcpy(term->buffer, term->buffer + TERM_W, TERM_W * (TERM_HISTORY - 1));
        
        memcpy(term->fg_colors, term->fg_colors + TERM_W, TERM_W * (TERM_HISTORY - 1) * 4);
        memcpy(term->bg_colors, term->bg_colors + TERM_W, TERM_W * (TERM_HISTORY - 1) * 4);
        memset(term->buffer + TERM_W * (TERM_HISTORY - 1), ' ', TERM_W);
        
        int start_fill = TERM_W * (TERM_HISTORY - 1);
        for(int i=0; i<TERM_W; i++) {
            term->fg_colors[start_fill + i] = term->curr_fg;
            term->bg_colors[start_fill + i] = term->curr_bg;
        }
        
        term->row = TERM_HISTORY - 1;
        if (term->view_row > 0) term->view_row--;
    }

    if (term->row > term->max_row) term->max_row = term->row;

    int at_bottom = (term->view_row + TERM_H) >= term->row;
    if (at_bottom) {
        if (term->row >= TERM_H) term->view_row = term->row - TERM_H + 1;
        else term->view_row = 0;
    }
}

void term_print(term_instance_t* term, const char* s) {
    while (*s) term_putc(term, *s++);
}

void vga_render_terminal_instance(term_instance_t* term, int win_x, int win_y) {
    for (int y = 0; y < TERM_H; y++) {
        for (int x = 0; x < TERM_W; x++) {
            char c = term->buffer[y * TERM_W + x];
            if (c != ' ') {
                vga_draw_char_sse(c, win_x + x * 8, win_y + y * 16, 0x1E1E1E);
            }
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
        memcpy(term_buffer, term_buffer + TERM_W, TERM_W * (TERM_H - 1));
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

    for (int cy = y1; cy < y2; cy++) {
        uint32_t* dest = &vga_current_target[cy * vga_target_w + x1];
        
        if (sse_width > 0) {
            uint32_t tmp_count = sse_width;
            uint32_t* tmp_dest = dest;
            __asm__ volatile (
                ".loop_rect:\n\t"
                "movups %%xmm0, (%0)\n\t"
                "add $16, %0\n\t"
                "dec %1\n\t"
                "jnz .loop_rect\n\t"
                : "+r"(tmp_dest), "+r"(tmp_count)
                :
                : "memory"
            );
            dest = tmp_dest;
        }
        
        for (uint32_t j = 0; j < remainder; j++) {
            dest[j] = color;
        }
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
    if (x >= (int)vga_target_w || y >= (int)vga_target_h) return;

    __asm__ volatile (
        "movd %0, %%xmm7\n\t"
        "pshufd $0, %%xmm7, %%xmm7\n\t" 
        : : "r"(trans_color) : "xmm7"
    );

    for (int i = 0; i < h; i++) {
        int cy = y + i;
        if (cy < 0 || cy >= (int)vga_target_h) continue;

        int current_draw_w = w;
        if (x + current_draw_w > (int)vga_target_w) {
            current_draw_w = (int)vga_target_w - x;
        }
        if (current_draw_w <= 0) continue;

        uint32_t* dst = &vga_current_target[cy * vga_target_w + x];
        uint32_t* src = &data[i * w];

        if (x + 4 > (int)vga_target_w || current_draw_w < 4) {
            for (int j = 0; j < current_draw_w; j++) {
                if (src[j] != trans_color) dst[j] = src[j];
            }
        } else {
            int sse_count = current_draw_w / 4;
            for (int j = 0; j < sse_count; j++) {
                __asm__ volatile (
                    "movups (%0), %%xmm0\n\t"
                    "movups (%1), %%xmm1\n\t"
                    "movaps %%xmm0, %%xmm2\n\t"
                    "pcmpeqd %%xmm7, %%xmm2\n\t"
                    "pand %%xmm2, %%xmm1\n\t"
                    "pandn %%xmm0, %%xmm2\n\t"
                    "por %%xmm2, %%xmm1\n\t"
                    "movups %%xmm1, (%1)\n\t"
                    : : "r"(src), "r"(dst) : "memory", "xmm0", "xmm1", "xmm2"
                );
                src += 4; dst += 4;
            }
            for (int j = 0; j < (current_draw_w % 4); j++) {
                if (src[j] != trans_color) dst[j] = src[j];
            }
        }
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

    for (int i = 0; i < h; i++) {
        int cy = y + i;
        if (cy < 0 || cy >= (int)vga_target_h) continue;
        uint32_t* dst_ptr = &vga_current_target[cy * vga_target_w + x];
        int count = w / 2; 

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
    }
}

__attribute__((target("sse2")))
void vga_draw_gradient_v(int x, int y, int w, int h, uint32_t c1, uint32_t c2) {
    if (!vga_current_target) return;
    for (int i = 0; i < h; i++) {
        uint8_t r = (((c1 >> 16) & 0xFF) * (h - i) + ((c2 >> 16) & 0xFF) * i) / h;
        uint8_t g = (((c1 >> 8) & 0xFF) * (h - i) + ((c2 >> 8) & 0xFF) * i) / h;
        uint8_t b = ((c1 & 0xFF) * (h - i) + (c2 & 0xFF) * i) / h;
        uint32_t color = (r << 16) | (g << 8) | b;
        
        vga_draw_rect(x, y + i, w, 1, color);
    }
}

__attribute__((target("sse2")))
void vga_blur_rect(int x, int y, int w, int h) {
    for (int i = 1; i < h - 1; i++) {
        uint32_t* row = &vga_current_target[(y + i) * vga_target_w + x];
        uint32_t* up  = &vga_current_target[(y + i - 1) * vga_target_w + x];
        uint32_t* down = &vga_current_target[(y + i + 1) * vga_target_w + x];

        for (int j = 0; j < w / 4; j++) {
            __asm__ volatile (
                "movups (%1), %%xmm0\n" 
                "movups (%2), %%xmm1\n" 
                "movups (%3), %%xmm2\n" 
                "pavgb %%xmm1, %%xmm0\n"
                "pavgb %%xmm2, %%xmm0\n"
                "movups %%xmm0, (%0)\n"
                : : "r"(row), "r"(row), "r"(up), "r"(row) : "memory", "xmm0", "xmm1", "xmm2"
            );
            row += 4; up += 4; down += 4;
        }
    }
}

__attribute__((target("sse2")))
void vga_blit_canvas(int x, int y, uint32_t* canvas, int w, int h) {
    if (!canvas) return;

    for (int i = 0; i < h; i++) {
        int screen_y = y + i;
        if (screen_y < 0 || screen_y >= (int)fb_height) continue;
        
        uint32_t* dst = &back_buffer[screen_y * fb_width + x];
        uint32_t* src = &canvas[i * w];
        
        int count = w / 4; 

        if (count > 0) {
            __asm__ volatile (
                "test %2, %2\n\t"
                "jz .end_blit_sse\n\t"
                ".loop_blit_sse:\n\t"
                "movups (%0), %%xmm0\n\t" 
                "movups %%xmm0, (%1)\n\t" 
                "add $16, %0\n\t"
                "add $16, %1\n\t"
                "dec %2\n\t"
                "jnz .loop_blit_sse\n\t"
                ".end_blit_sse:\n\t"
                : "+r"(src), "+r"(dst), "+r"(count)
                : 
                : "memory", "xmm0", "cc"
            );
        }

        int remainder = w % 4;
        for (int j = 0; j < remainder; j++) {
            dst[j] = src[j];
        }
    }
    vga_mark_dirty(x, y, w, h);
}

void vga_draw_sprite_scaled_masked(int x, int y, int sw, int sh, int scale, uint32_t* data, uint32_t trans) {
    if (!vga_current_target) return;

    for (int i = 0; i < sh; i++) {
        int cy = y + i * scale;
        if (cy >= (int)vga_target_h) break;
        if (cy + scale <= 0) continue;

        for (int j = 0; j < sw; j++) {
            uint32_t color = data[i * sw + j];
            if (color == trans) continue;

            int cx = x + j * scale;
            if (cx >= (int)vga_target_w) break;
            if (cx + scale <= 0) continue;

            vga_draw_rect(cx, cy, scale, scale, color);
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