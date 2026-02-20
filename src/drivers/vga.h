// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#ifndef DRIVERS_VGA_H
#define DRIVERS_VGA_H

#include <hal/lock.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum VgaColor {
    COLOR_BLACK      = 0x000000,
    COLOR_WHITE      = 0xFFFFFF,
    COLOR_LIGHT_GREY = 0xAAAAAA,
    COLOR_LIGHT_GREEN= 0x00FF00,
    COLOR_RED        = 0xFF0000,
    COLOR_BLUE       = 0x0000FF,
};

void vga_init(void);
void vga_init_graphics(void);
void vga_reset_dirty(void);
void vga_flip_dirty(void);
void vga_present_rect(const void* src, uint32_t src_stride, int x, int y, int w, int h);
void vga_clear_terminal(void);

void vga_putc(char c);
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
void vga_set_target(uint32_t* target, uint32_t w, uint32_t h);
void vga_mark_dirty(int x, int y, int w, int h);
void vga_draw_char_sse(int x, int y, char c, uint32_t fg);
int vga_is_rect_dirty(int x, int y, int w, int h);

#ifdef __cplusplus
}
#endif

#endif
