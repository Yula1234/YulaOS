// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#ifndef DRIVERS_VGA_H
#define DRIVERS_VGA_H

#include <hal/lock.h>

#include <stdint.h>

enum VgaColor {
    COLOR_BLACK      = 0x000000,
    COLOR_WHITE      = 0xFFFFFF,
    COLOR_LIGHT_GREY = 0xAAAAAA,
    COLOR_LIGHT_GREEN= 0x00FF00,
    COLOR_RED        = 0xFF0000,
    COLOR_BLUE       = 0x0000FF,
};

    
#define TERM_W 80
#define TERM_H 12


typedef struct {
    char* buffer;
    
    uint32_t* fg_colors;
    uint32_t* bg_colors;

    int history_cap_rows;
    int history_rows;

    uint32_t curr_fg;
    uint32_t curr_bg;
    uint32_t def_fg;
    uint32_t def_bg;

    int cols;
    int view_rows;
 
    int col;
    int row;
    int view_row;
    int max_row;

    int saved_col;
    int saved_row;
    int esc_state;
    int csi_in_param;
    int csi_param_value;
    int csi_param_count;
    int csi_params[8];
    int ansi_bright;
    int ansi_inverse;

    spinlock_t lock;
} term_instance_t;

void vga_render_terminal_instance(term_instance_t* term, int win_x, int win_y);
void term_init(term_instance_t* term);
void term_destroy(term_instance_t* term);
void term_putc(term_instance_t* term, char c);
void term_write(term_instance_t* term, const char* buf, uint32_t len);
void term_print(term_instance_t* term, const char* s);
void term_reflow(term_instance_t* term, int new_cols);
void term_clear_row(term_instance_t* term, int row);
void term_get_cell(term_instance_t* term, int row, int col, char* out_ch, uint32_t* out_fg, uint32_t* out_bg);
void term_set_cell(term_instance_t* term, int row, int col, char ch, uint32_t fg, uint32_t bg);

void vga_init(void);
void vga_init_graphics(void);
void vga_reset_dirty(void);
void vga_flip_dirty(void);
void vga_present_rect(const void* src, uint32_t src_stride, int x, int y, int w, int h);
void vga_clear_terminal(void);
void vga_flip(void);

void vga_putc(char c);
void vga_print_u32(uint32_t v);
void vga_print(const char* s);
void vga_clear(uint32_t color);
void vga_set_color(uint32_t fg, uint32_t bg);
void vga_put_pixel(uint32_t x, uint32_t y, uint32_t color);
void vga_draw_rect(int x, int y, int w, int h, uint32_t color);
void vga_put_pixel(uint32_t x, uint32_t y, uint32_t color);
void vga_draw_cursor(int x, int y);
void vga_render_terminal(int win_x, int win_y);
void vga_print_at(const char* s, int x, int y, uint32_t fg);
void vga_flip_rect(int x, int y, int w, int h);
void term_print_u32(term_instance_t* term, uint32_t n);
void vga_draw_rect_alpha(int x, int y, int w, int h, uint32_t color, uint8_t alpha);
void vga_draw_gradient_v(int x, int y, int w, int h, uint32_t c1, uint32_t c2);
void vga_blur_rect(int x, int y, int w, int h);
void vga_blit_canvas(int dest_x, int dest_y, uint32_t* src_canvas, int src_w, int src_h);
void vga_set_target(uint32_t* target, uint32_t w, uint32_t h);
void vga_draw_sprite_masked(int x, int y, int w, int h, uint32_t* data, uint32_t trans_color);
void vga_draw_sprite_scaled_masked(int x, int y, int sw, int sh, int scale, uint32_t* data, uint32_t trans);
void vga_mark_dirty(int x, int y, int w, int h);
void vga_draw_char_sse(int x, int y, char c, uint32_t fg);
int vga_is_rect_dirty(int x, int y, int w, int h);

#endif
