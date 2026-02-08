// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#pragma once

#include "paint_state.h"
#include "paint_util.h"

void fill_rect_raw(uint32_t* dst, int dst_w, int dst_h, int x, int y, int w, int h, uint32_t color);
void fill_rect(int x, int y, int w, int h, uint32_t color);
void fill_rect_alpha(int x, int y, int w, int h, uint32_t color, uint8_t alpha);
void draw_frame(int x, int y, int w, int h, uint32_t color);
int pt_in_rect(int x, int y, rect_t r);

void canvas_put_pixel_alpha(int x, int y, uint32_t color, uint8_t alpha);
void canvas_draw_disc_alpha_img(int cx, int cy, int r, uint32_t color, uint8_t alpha);
void canvas_draw_line_alpha_img(int x0, int y0, int x1, int y1, int r, uint32_t color, uint8_t alpha);
void canvas_draw_rect_alpha_img(int x0, int y0, int x1, int y1, int r, uint32_t color, int fill, uint8_t alpha);
void canvas_draw_circle_alpha_img(int x0, int y0, int x1, int y1, int r, uint32_t color, int fill, uint8_t alpha);
