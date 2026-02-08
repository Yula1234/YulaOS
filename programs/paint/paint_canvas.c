// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#include "paint_canvas.h"

void fill_rect_raw(uint32_t* dst, int dst_w, int dst_h, int x, int y, int w, int h, uint32_t color) {
    for (int j = 0; j < h; j++) {
        int py = y + j;
        if ((unsigned)py >= (unsigned)dst_h) {
            continue;
        }
        uint32_t* row = dst + py * dst_w;
        for (int i = 0; i < w; i++) {
            int px = x + i;
            if ((unsigned)px >= (unsigned)dst_w) {
                continue;
            }
            row[px] = color;
        }
    }
}

void fill_rect(int x, int y, int w, int h, uint32_t color) {
    fill_rect_raw(canvas, WIN_W, WIN_H, x, y, w, h, color);
}

void fill_rect_alpha(int x, int y, int w, int h, uint32_t color, uint8_t alpha) {
    for (int j = 0; j < h; j++) {
        int py = y + j;
        if ((unsigned)py >= (unsigned)WIN_H) {
            continue;
        }
        uint32_t* row = canvas + py * WIN_W;
        for (int i = 0; i < w; i++) {
            int px = x + i;
            if ((unsigned)px >= (unsigned)WIN_W) {
                continue;
            }
            row[px] = blend(color, row[px], alpha);
        }
    }
}

void draw_frame(int x, int y, int w, int h, uint32_t color) {
    fill_rect(x, y, w, 1, color);
    fill_rect(x, y + h - 1, w, 1, color);
    fill_rect(x, y, 1, h, color);
    fill_rect(x + w - 1, y, 1, h, color);
}

int pt_in_rect(int x, int y, rect_t r) {
    if (x < r.x) {
        return 0;
    }
    if (x >= r.x + r.w) {
        return 0;
    }
    if (y < r.y) {
        return 0;
    }
    if (y >= r.y + r.h) {
        return 0;
    }
    return 1;
}

void canvas_put_pixel_alpha(int x, int y, uint32_t color, uint8_t alpha) {
    if ((unsigned)x >= (unsigned)WIN_W) {
        return;
    }
    if ((unsigned)y >= (unsigned)WIN_H) {
        return;
    }
    uint32_t* p = &canvas[y * WIN_W + x];
    *p = blend(color, *p, alpha);
}

void canvas_draw_disc_alpha_img(int cx, int cy, int r, uint32_t color, uint8_t alpha) {
    int ox = r_canvas.x;
    int oy = r_canvas.y;
    if (r <= 0) {
        canvas_put_pixel_alpha(ox + cx, oy + cy, color, alpha);
        return;
    }
    int rr = r * r;
    for (int dy2 = -r; dy2 <= r; dy2++) {
        for (int dx2 = -r; dx2 <= r; dx2++) {
            if (dx2 * dx2 + dy2 * dy2 <= rr) {
                canvas_put_pixel_alpha(ox + cx + dx2, oy + cy + dy2, color, alpha);
            }
        }
    }
}

void canvas_draw_line_alpha_img(int x0, int y0, int x1, int y1, int r, uint32_t color, uint8_t alpha) {
    int dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int sx = (x0 < x1) ? 1 : -1;
    int dy = (y1 > y0) ? (y0 - y1) : (y1 - y0);
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx + dy;
    while (1) {
        canvas_draw_disc_alpha_img(x0, y0, r, color, alpha);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int e2 = err * 2;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void canvas_draw_rect_alpha_img(int x0, int y0, int x1, int y1, int r, uint32_t color, int fill, uint8_t alpha) {
    int lx = min_i(x0, x1);
    int rx = max_i(x0, x1);
    int ty = min_i(y0, y1);
    int by = max_i(y0, y1);
    if (fill) {
        uint8_t a2 = (alpha > 70) ? (uint8_t)(alpha - 70) : (uint8_t)(alpha / 2);
        for (int y = ty; y <= by; y++) {
            canvas_draw_line_alpha_img(lx, y, rx, y, 0, color, a2);
        }
    }
    canvas_draw_line_alpha_img(lx, ty, rx, ty, r, color, alpha);
    canvas_draw_line_alpha_img(lx, by, rx, by, r, color, alpha);
    canvas_draw_line_alpha_img(lx, ty, lx, by, r, color, alpha);
    canvas_draw_line_alpha_img(rx, ty, rx, by, r, color, alpha);
}

void canvas_draw_circle_alpha_img(int x0, int y0, int x1, int y1, int r, uint32_t color, int fill, uint8_t alpha) {
    int cx = x0;
    int cy = y0;
    int dx = x1 - x0;
    int dy = y1 - y0;
    int rad = isqrt_i(dx * dx + dy * dy);
    if (fill) {
        uint8_t a2 = (alpha > 70) ? (uint8_t)(alpha - 70) : (uint8_t)(alpha / 2);
        int rr = rad * rad;
        for (int yy = -rad; yy <= rad; yy++) {
            int span = isqrt_i(rr - yy * yy);
            canvas_draw_line_alpha_img(cx - span, cy + yy, cx + span, cy + yy, 0, color, a2);
        }
    }
    if (rad <= 0) {
        canvas_draw_disc_alpha_img(cx, cy, r, color, alpha);
        return;
    }
    int x = rad;
    int y = 0;
    int err = 1 - x;
    while (x >= y) {
        canvas_draw_disc_alpha_img(cx + x, cy + y, r, color, alpha);
        canvas_draw_disc_alpha_img(cx + y, cy + x, r, color, alpha);
        canvas_draw_disc_alpha_img(cx - y, cy + x, r, color, alpha);
        canvas_draw_disc_alpha_img(cx - x, cy + y, r, color, alpha);
        canvas_draw_disc_alpha_img(cx - x, cy - y, r, color, alpha);
        canvas_draw_disc_alpha_img(cx - y, cy - x, r, color, alpha);
        canvas_draw_disc_alpha_img(cx + y, cy - x, r, color, alpha);
        canvas_draw_disc_alpha_img(cx + x, cy - y, r, color, alpha);
        y++;
        if (err < 0) {
            err += 2 * y + 1;
        } else {
            x--;
            err += 2 * (y - x + 1);
        }
    }
}
