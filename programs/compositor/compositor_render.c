#include "compositor_internal.h"

static inline void put_pixel(uint32_t* fb, int stride, int w, int h, int x, int y, uint32_t color) {
    if ((unsigned)x >= (unsigned)w || (unsigned)y >= (unsigned)h) return;
    fb[(size_t)y * (size_t)stride + (size_t)x] = color;
}

void fill_rect(uint32_t* fb, int stride, int w, int h, int x, int y, int rw, int rh, uint32_t color) {
    if (rw <= 0 || rh <= 0) return;

    int x0 = x;
    int y0 = y;
    int x1 = x + rw;
    int y1 = y + rh;

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > w) x1 = w;
    if (y1 > h) y1 = h;

    if (x0 >= x1 || y0 >= y1) return;

    for (int yy = y0; yy < y1; yy++) {
        uint32_t* row = fb + (size_t)yy * (size_t)stride;

        int xx = x0;
        int n = x1 - x0;
        while (n >= 8) {
            row[xx + 0] = color;
            row[xx + 1] = color;
            row[xx + 2] = color;
            row[xx + 3] = color;
            row[xx + 4] = color;
            row[xx + 5] = color;
            row[xx + 6] = color;
            row[xx + 7] = color;
            xx += 8;
            n -= 8;
        }
        while (n--) {
            row[xx++] = color;
        }
    }
}

static inline void put_pixel_clipped(uint32_t* fb, int stride, int w, int h, int x, int y, uint32_t color, comp_rect_t clip) {
    if (x < clip.x1 || x >= clip.x2 || y < clip.y1 || y >= clip.y2) return;
    put_pixel(fb, stride, w, h, x, y, color);
}

static inline void fill_rect_clipped(uint32_t* fb, int stride, int w, int h, int x, int y, int rw, int rh, uint32_t color, comp_rect_t clip) {
    comp_rect_t r = rect_make(x, y, rw, rh);
    r = rect_intersect(r, clip);
    if (rect_empty(&r)) return;
    fill_rect(fb, stride, w, h, r.x1, r.y1, r.x2 - r.x1, r.y2 - r.y1, color);
}

void draw_cursor_clipped(uint32_t* fb, int stride, int w, int h, int x, int y, comp_rect_t clip) {
    const uint32_t c1 = 0xFFFFFF;
    const uint32_t c2 = 0x000000;

    for (int i = -7; i <= 7; i++) {
        put_pixel_clipped(fb, stride, w, h, x + i, y, c1, clip);
        put_pixel_clipped(fb, stride, w, h, x, y + i, c1, clip);
        put_pixel_clipped(fb, stride, w, h, x + i, y + 1, c2, clip);
        put_pixel_clipped(fb, stride, w, h, x + 1, y + i, c2, clip);
    }

    fill_rect_clipped(fb, stride, w, h, x - 1, y - 1, 3, 3, 0xFF0000, clip);
}

void draw_frame_rect_clipped(uint32_t* fb, int stride, int w, int h, int x, int y, int rw, int rh, int t, uint32_t color, comp_rect_t clip) {
    if (!fb) return;
    if (rw <= 0 || rh <= 0) return;
    if (t <= 0) return;
    if (rw <= t * 2 || rh <= t * 2) return;

    fill_rect_clipped(fb, stride, w, h, x, y, rw, t, color, clip);
    fill_rect_clipped(fb, stride, w, h, x, y + rh - t, rw, t, color, clip);
    fill_rect_clipped(fb, stride, w, h, x, y, t, rh, color, clip);
    fill_rect_clipped(fb, stride, w, h, x + rw - t, y, t, rh, color, clip);
}

void blit_surface_clipped(uint32_t* dst, int dst_stride, int dst_w, int dst_h, int dx, int dy,
                          const uint32_t* src, int src_stride, int src_w, int src_h, comp_rect_t clip) {
    if (!dst || !src) return;
    if (dst_w <= 0 || dst_h <= 0 || src_w <= 0 || src_h <= 0) return;

    comp_rect_t srect = rect_make(dx, dy, src_w, src_h);
    comp_rect_t drect = rect_make(0, 0, dst_w, dst_h);
    comp_rect_t r = rect_intersect(srect, clip);
    r = rect_intersect(r, drect);
    if (rect_empty(&r)) return;

    int off_x = r.x1 - dx;
    int off_y = r.y1 - dy;
    int copy_w = r.x2 - r.x1;
    int copy_h = r.y2 - r.y1;

    for (int y = 0; y < copy_h; y++) {
        uint32_t* drow = dst + (size_t)(r.y1 + y) * (size_t)dst_stride + (size_t)r.x1;
        const uint32_t* srow = src + (size_t)(off_y + y) * (size_t)src_stride + (size_t)off_x;
        memcpy(drow, srow, (size_t)copy_w * 4u);
    }
}

void present_damage_to_fb(uint32_t* fb, const uint32_t* src, int stride, comp_damage_t* dmg) {
    if (!fb || !src || !dmg || dmg->n <= 0) return;

    for (int ri = 0; ri < dmg->n; ri++) {
        const comp_rect_t r = dmg->rects[ri];
        const int w = r.x2 - r.x1;
        const int h = r.y2 - r.y1;
        if (w <= 0 || h <= 0) continue;

        for (int y = r.y1; y < r.y2; y++) {
            uint32_t* drow = fb + (size_t)y * (size_t)stride + (size_t)r.x1;
            const uint32_t* srow = src + (size_t)y * (size_t)stride + (size_t)r.x1;
            memcpy(drow, srow, (size_t)w * 4u);
        }
    }
}
