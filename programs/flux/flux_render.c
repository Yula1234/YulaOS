#include "flux_internal.h"
#include "flux_cursor.h"

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

void draw_text(uint32_t* fb, int stride, int w, int h, int x, int y, const char* s, uint32_t color) {
    if (!fb || !s) return;
    if (stride <= 0 || w <= 0 || h <= 0) return;

    int cx = x;
    const char* p = s;
    while (*p) {
        unsigned char uc = (unsigned char)*p;
        if (uc >= 128u) uc = (unsigned char)'?';
        const uint8_t* glyph = font8x8_basic[(int)uc];

        for (int row = 0; row < 8; row++) {
            int py = y + row;
            if (py < 0 || py >= h) continue;
            uint8_t bits = glyph[row];
            if (!bits) continue;
            uint32_t* rowp = fb + (size_t)py * (size_t)stride;
            for (int col = 0; col < 8; col++) {
                if ((bits & (uint8_t)(1u << (7 - col))) == 0u) continue;
                int px = cx + col;
                if (px < 0 || px >= w) continue;
                rowp[px] = color;
            }
        }

        cx += 8;
        if (cx >= w) break;
        p++;
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

#ifndef COMP_CURSOR_SAVE_W
#define COMP_CURSOR_SAVE_W 17
#endif
#ifndef COMP_CURSOR_SAVE_H
#define COMP_CURSOR_SAVE_H 17
#endif
#ifndef COMP_CURSOR_HOTSPOT_X
#define COMP_CURSOR_HOTSPOT_X 0
#endif
#ifndef COMP_CURSOR_HOTSPOT_Y
#define COMP_CURSOR_HOTSPOT_Y 0
#endif

static uint32_t g_under_cursor_save[COMP_CURSOR_SAVE_W * COMP_CURSOR_SAVE_H];
static int g_under_cursor_valid = 0;
static int g_under_cursor_x = 0;
static int g_under_cursor_y = 0;

typedef struct {
    uint32_t* fb;
    int stride;
    int w;
    int h;
    comp_rect_t clip;
} flux_cursor_draw_fb_ctx_t;

static int flux_cursor_draw_fb_cb(void* raw, int rx, int ry, int rw, int rh, int color_type) {
    flux_cursor_draw_fb_ctx_t* c = (flux_cursor_draw_fb_ctx_t*)raw;
    uint32_t color = (color_type == 0) ? 0x000000u : 0xFFFFFFu;
    fill_rect_clipped(c->fb, c->stride, c->w, c->h, rx, ry, rw, rh, color, c->clip);
    return 0;
}

void comp_cursor_reset(void) {
    g_under_cursor_valid = 0;
    g_under_cursor_x = 0;
    g_under_cursor_y = 0;
}

void draw_cursor_clipped(uint32_t* fb, int stride, int w, int h, int x, int y, comp_rect_t clip) {
    flux_cursor_draw_fb_ctx_t ctx;
    ctx.fb = fb;
    ctx.stride = stride;
    ctx.w = w;
    ctx.h = h;
    ctx.clip = clip;

    flux_cursor_draw_arrow(&ctx, x - COMP_CURSOR_HOTSPOT_X, y - COMP_CURSOR_HOTSPOT_Y, flux_cursor_draw_fb_cb);
}

void comp_cursor_restore(uint32_t* fb, int stride, int w, int h) {
    if (!fb) return;
    if (stride <= 0 || w <= 0 || h <= 0) return;
    if (!g_under_cursor_valid) return;

    const int x0 = g_under_cursor_x - COMP_CURSOR_HOTSPOT_X;
    const int y0 = g_under_cursor_y - COMP_CURSOR_HOTSPOT_Y;

    for (int yy = 0; yy < COMP_CURSOR_SAVE_H; yy++) {
        const int y = y0 + yy;
        if ((unsigned)y >= (unsigned)h) continue;
        uint32_t* row = fb + (size_t)y * (size_t)stride;
        const uint32_t* srow = g_under_cursor_save + (size_t)yy * (size_t)COMP_CURSOR_SAVE_W;
        for (int xx = 0; xx < COMP_CURSOR_SAVE_W; xx++) {
            const int x = x0 + xx;
            if ((unsigned)x >= (unsigned)w) continue;
            row[x] = srow[xx];
        }
    }

    g_under_cursor_valid = 0;
}

void comp_cursor_save_under_draw(uint32_t* fb, int stride, int w, int h, int x, int y) {
    if (!fb) return;
    if (stride <= 0 || w <= 0 || h <= 0) return;

    const int x0 = x - COMP_CURSOR_HOTSPOT_X;
    const int y0 = y - COMP_CURSOR_HOTSPOT_Y;

    for (int yy = 0; yy < COMP_CURSOR_SAVE_H; yy++) {
        const int sy = y0 + yy;
        uint32_t* drow = g_under_cursor_save + (size_t)yy * (size_t)COMP_CURSOR_SAVE_W;

        if ((unsigned)sy >= (unsigned)h) {
            for (int xx = 0; xx < COMP_CURSOR_SAVE_W; xx++) {
                drow[xx] = 0;
            }
            continue;
        }

        const uint32_t* srow = fb + (size_t)sy * (size_t)stride;
        for (int xx = 0; xx < COMP_CURSOR_SAVE_W; xx++) {
            const int sx = x0 + xx;
            if ((unsigned)sx >= (unsigned)w) {
                drow[xx] = 0;
            } else {
                drow[xx] = srow[sx];
            }
        }
    }

    g_under_cursor_x = x;
    g_under_cursor_y = y;
    g_under_cursor_valid = 1;

    draw_cursor_clipped(fb, stride, w, h, x, y, rect_make(0, 0, w, h));
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
