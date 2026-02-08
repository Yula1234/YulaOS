// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#pragma once

#include "paint_state.h"
#include "paint_util.h"

size_t img_pixel_count(void);
void img_resize_to_canvas(void);

void img_put_pixel(int x, int y, uint32_t color);
void img_draw_disc(int cx, int cy, int r, uint32_t color);
void img_draw_line(int x0, int y0, int x1, int y1, int r, uint32_t color);
void img_fill_rect(int x0, int y0, int x1, int y1, uint32_t color);
void img_draw_rect(int x0, int y0, int x1, int y1, int r, uint32_t color, int fill);
void img_fill_circle(int cx, int cy, int rad, uint32_t color);
void img_draw_circle(int x0, int y0, int x1, int y1, int r, uint32_t color, int fill);
void flood_fill(int sx, int sy, uint32_t target, uint32_t repl);

void snapshot_init(snapshot_t* s, char tag);
void snapshot_free(snapshot_t* s);
void push_undo(void);
void do_undo(void);
void do_redo(void);
