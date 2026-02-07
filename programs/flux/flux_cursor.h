#pragma once

#include <stdint.h>

typedef int (*flux_cursor_draw_rect_fn)(void* ctx, int x, int y, int w, int h, int color_type);

int flux_cursor_draw_arrow(void* ctx, int x, int y, flux_cursor_draw_rect_fn draw_fn);
