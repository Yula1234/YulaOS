// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#include <yula.h>
#include <comp.h>
#include <font.h>

static inline void dbg_write(const char* s) {
    if (!s) return;
    write(1, s, (uint32_t)strlen(s));
}

static inline int ptr_is_invalid(const void* p) {
    return !p || p == (const void*)-1;
}

static int WIN_W = 800;
static int WIN_H = 600;

#define SIGINT  2
#define SIGILL  4
#define SIGSEGV 11
#define SIGTERM 15

static volatile int g_dbg_stage;
static volatile int32_t g_dbg_resize_w;
static volatile int32_t g_dbg_resize_h;

static void paint_on_signal(int sig) {
    char tmp[160];
    (void)snprintf(tmp, sizeof(tmp), "paint: signal=%d stage=%d win=%dx%d resize=%dx%d\n",
                   sig,
                   (int)g_dbg_stage,
                   WIN_W,
                   WIN_H,
                   (int)g_dbg_resize_w,
                   (int)g_dbg_resize_h);
    dbg_write(tmp);
    syscall(0, 128 + (uint32_t)sig, 0, 0);
}


#define PAINT_MAX_SURFACE_BYTES (32u * 1024u * 1024u)
#define PAINT_MAX_IMG_BYTES     (16u * 1024u * 1024u)

#define PAINT_MAX_SURFACE_PIXELS (PAINT_MAX_SURFACE_BYTES / 4u)
#define PAINT_MAX_IMG_PIXELS     (PAINT_MAX_IMG_BYTES / 4u)

#define C_WIN_BG    0x1E1E1E
#define C_PANEL_BG  0x252526
#define C_HEADER_BG 0x2D2D2D
#define C_BORDER    0x3E3E42
#define C_TEXT      0xD4D4D4
#define C_TEXT_DIM  0x9A9A9A
#define C_ACCENT    0x007ACC
#define C_CANVAS_BG 0xFFFFFF

#define UI_TOP_H    36
#define UI_STATUS_H 22
#define UI_TOOL_W   96

typedef struct { int x, y, w, h; } rect_t;

enum {
    TOOL_BRUSH = 0,
    TOOL_ERASER = 1,
    TOOL_LINE = 2,
    TOOL_RECT = 3,
    TOOL_CIRCLE = 4,
    TOOL_FILL = 5,
    TOOL_PICK = 6,
};

typedef struct {
    uint32_t* pixels;
    int w;
    int h;
} snapshot_t;

static uint32_t* canvas;

static rect_t r_header;
static rect_t r_toolbar;
static rect_t r_status;
static rect_t r_canvas;

static uint32_t* img;
static int img_w;
static int img_h;
 static int img_shm_fd = -1;
 static char img_shm_name[32];
 static uint32_t img_cap_bytes;
 static int img_shm_gen;

static inline size_t img_pixel_count(void) {
    if (ptr_is_invalid(img)) return 0;
    if (img_w <= 0 || img_h <= 0) return 0;
    size_t c = (size_t)(uint32_t)img_w * (size_t)(uint32_t)img_h;
    if (c == 0 || c > (size_t)PAINT_MAX_IMG_PIXELS) return 0;
    return c;
}

static const uint32_t palette[8] = {
    0x000000,
    0xFFFFFF,
    0xFF0000,
    0x00FF00,
    0x0000FF,
    0xFFFF00,
    0xFF00FF,
    0x00FFFF,
};

static int tool = TOOL_BRUSH;
static int brush_r = 2;
static uint32_t cur_color = 0x111111;
static int shape_fill = 0;

static int mouse_down;
static int drag_active;
static int drag_start_x;
static int drag_start_y;
static int drag_cur_x;
static int drag_cur_y;
static int last_img_x;
static int last_img_y;

static snapshot_t undo_stack[1];
static int undo_count;

static snapshot_t redo_stack[1];
static int redo_count;

static int min_i(int a, int b) { return a < b ? a : b; }
static int max_i(int a, int b) { return a > b ? a : b; }

static int isqrt_i(int v) {
    if (v <= 0) return 0;
    int x = v;
    int y = (x + 1) / 2;
    while (y < x) {
        x = y;
        y = (x + v / x) / 2;
    }
    return x;
}

static uint32_t blend(uint32_t fg, uint32_t bg, uint8_t alpha) {
    if (alpha == 255) return fg;
    if (alpha == 0) return bg;
    uint32_t r = ((fg >> 16) & 0xFF) * alpha + ((bg >> 16) & 0xFF) * (255 - alpha);
    uint32_t g = ((fg >> 8) & 0xFF) * alpha + ((bg >> 8) & 0xFF) * (255 - alpha);
    uint32_t b = (fg & 0xFF) * alpha + (bg & 0xFF) * (255 - alpha);
    return ((r >> 8) << 16) | ((g >> 8) << 8) | (b >> 8);
}

static void fill_rect_raw(uint32_t* dst, int dst_w, int dst_h, int x, int y, int w, int h, uint32_t color) {
    for (int j = 0; j < h; j++) {
        int py = y + j;
        if ((unsigned)py >= (unsigned)dst_h) continue;
        uint32_t* row = dst + py * dst_w;
        for (int i = 0; i < w; i++) {
            int px = x + i;
            if ((unsigned)px >= (unsigned)dst_w) continue;
            row[px] = color;
        }
    }
}

static void fill_rect(int x, int y, int w, int h, uint32_t color) {
    fill_rect_raw(canvas, WIN_W, WIN_H, x, y, w, h, color);
}

static void fill_rect_alpha(int x, int y, int w, int h, uint32_t color, uint8_t alpha) {
    for (int j = 0; j < h; j++) {
        int py = y + j;
        if ((unsigned)py >= (unsigned)WIN_H) continue;
        uint32_t* row = canvas + py * WIN_W;
        for (int i = 0; i < w; i++) {
            int px = x + i;
            if ((unsigned)px >= (unsigned)WIN_W) continue;
            row[px] = blend(color, row[px], alpha);
        }
    }
}

static void draw_frame(int x, int y, int w, int h, uint32_t color) {
    fill_rect(x, y, w, 1, color);
    fill_rect(x, y + h - 1, w, 1, color);
    fill_rect(x, y, 1, h, color);
    fill_rect(x + w - 1, y, 1, h, color);
}

static int pt_in_rect(int x, int y, rect_t r) {
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

static int snapshot_reserve(snapshot_t* s, int w, int h) {
    if (!s || w <= 0 || h <= 0) return 0;
    if (s->pixels && !ptr_is_invalid(s->pixels) && s->w == w && s->h == h) return 1;

    uint64_t bytes64 = (uint64_t)(uint32_t)w * (uint64_t)(uint32_t)h * 4ull;
    if (bytes64 == 0 || bytes64 > (uint64_t)PAINT_MAX_IMG_BYTES) return 0;
    if (bytes64 > (uint64_t)(size_t)-1) return 0;
    size_t bytes = (size_t)bytes64;

    g_dbg_stage = 3165;
    void* p = malloc(bytes);
    if (ptr_is_invalid(p)) return 0;
    g_dbg_stage = 3166;

    if (s->pixels && !ptr_is_invalid(s->pixels)) {
        free(s->pixels);
    }

    s->pixels = (uint32_t*)p;
    s->w = w;
    s->h = h;
    return 1;
}

static void snapshot_free(snapshot_t* s) {
    if (!s) return;
    if (s->pixels && !ptr_is_invalid(s->pixels)) free(s->pixels);
    s->pixels = 0;
    s->w = 0;
    s->h = 0;
}

static int snapshot_capture(snapshot_t* out) {
    g_dbg_stage = 3161;
    size_t count = img_pixel_count();
    if (!out || count == 0) return 0;
    g_dbg_stage = 3162;
    if (!snapshot_reserve(out, img_w, img_h)) return 0;
    size_t bytes = count * 4u;
    if (bytes == 0 || bytes > (size_t)PAINT_MAX_IMG_BYTES) return 0;
    g_dbg_stage = 3167;
    memcpy(out->pixels, img, bytes);
    g_dbg_stage = 3168;
    return 1;
}

static void img_swap_with_snapshot(snapshot_t* s) {
    if (!s || ptr_is_invalid(img)) return;
    if (ptr_is_invalid(s->pixels)) return;
    if (s->w != img_w || s->h != img_h) return;

    size_t count = img_pixel_count();
    if (count == 0) return;
    for (size_t i = 0; i < count; i++) {
        uint32_t t = img[i];
        img[i] = s->pixels[i];
        s->pixels[i] = t;
    }
}

static void clear_redo() {
    redo_count = 0;
}

static void push_undo() {
    g_dbg_stage = 316;
    g_dbg_stage = 3160;
    if (!snapshot_capture(&undo_stack[0])) return;
    undo_count = 1;
    clear_redo();
    g_dbg_stage = 317;
}

static void do_undo() {
    if (undo_count <= 0 || !img) return;

    img_swap_with_snapshot(&undo_stack[0]);
    redo_count = 1;
    undo_count = 0;
}

static void do_redo() {
    if (redo_count <= 0 || !img) return;

    img_swap_with_snapshot(&undo_stack[0]);
    undo_count = 1;
    redo_count = 0;
}

static void layout_update() {
    r_header = (rect_t){0, 0, WIN_W, UI_TOP_H};
    r_status = (rect_t){0, WIN_H - UI_STATUS_H, WIN_W, UI_STATUS_H};

    int middle_h = WIN_H - UI_TOP_H - UI_STATUS_H;
    if (middle_h < 0) middle_h = 0;

    r_toolbar = (rect_t){0, UI_TOP_H, UI_TOOL_W, middle_h};
    r_canvas = (rect_t){UI_TOOL_W, UI_TOP_H, WIN_W - UI_TOOL_W, middle_h};

    if (r_canvas.w < 0) r_canvas.w = 0;
    if (r_canvas.h < 0) r_canvas.h = 0;
}

static void img_resize_to_canvas() {
    int new_w = r_canvas.w;
    int new_h = r_canvas.h;
    if (new_w <= 0 || new_h <= 0) return;

    if (!ptr_is_invalid(img) && img && img_w == new_w && img_h == new_h) return;

    const uint32_t max_img_pixels = (uint32_t)PAINT_MAX_IMG_PIXELS;
    if (max_img_pixels == 0) return;

    uint64_t want_pixels64 = (uint64_t)(uint32_t)new_w * (uint64_t)(uint32_t)new_h;
    if (want_pixels64 > (uint64_t)max_img_pixels) {
        if (new_w >= new_h) {
            new_w = (int)((uint32_t)max_img_pixels / (uint32_t)new_h);
        } else {
            new_h = (int)((uint32_t)max_img_pixels / (uint32_t)new_w);
        }
        if (new_w <= 0 || new_h <= 0) return;
    }

    uint64_t bytes64 = (uint64_t)(uint32_t)new_w * (uint64_t)(uint32_t)new_h * 4ull;
    if (bytes64 == 0 || bytes64 > 0xFFFFFFFFu) return;
    uint32_t bytes = (uint32_t)bytes64;

    const int old_w = img_w;
    const int old_h = img_h;

    const int can_reuse = (img && !ptr_is_invalid(img) && img_shm_fd >= 0 && bytes <= img_cap_bytes && new_w == old_w);
    if (can_reuse) {
        img_h = new_h;

        if (new_h > old_h) {
            for (int y = old_h; y < new_h; y++) {
                uint32_t* row = img + (size_t)(uint32_t)y * (size_t)(uint32_t)old_w;
                for (int x = 0; x < old_w; x++) {
                    row[x] = C_CANVAS_BG;
                }
            }
        }
    } else {
        if (bytes > (uint32_t)PAINT_MAX_IMG_BYTES) return;

        char new_name[32];
        new_name[0] = '\0';
        int new_fd = -1;
        const int pid = getpid();
        for (int i = 0; i < 16; i++) {
            img_shm_gen++;
            (void)snprintf(new_name, sizeof(new_name), "paintimg_%d_%d", pid, img_shm_gen);
            new_fd = shm_create_named(new_name, bytes);
            if (new_fd >= 0) break;
        }
        if (new_fd < 0) return;

        uint32_t* nimg = (uint32_t*)mmap(new_fd, bytes, MAP_SHARED);
        if (ptr_is_invalid(nimg)) {
            close(new_fd);
            shm_unlink_named(new_name);
            return;
        }

        size_t count = (size_t)(uint32_t)new_w * (size_t)(uint32_t)new_h;
        for (size_t i = 0; i < count; i++) nimg[i] = C_CANVAS_BG;

        if (img && !ptr_is_invalid(img) && old_w > 0 && old_h > 0) {
            int cw = min_i(old_w, new_w);
            int ch = min_i(old_h, new_h);
            for (int y = 0; y < ch; y++) {
                memcpy(nimg + (size_t)(uint32_t)y * (size_t)(uint32_t)new_w,
                       img + (size_t)(uint32_t)y * (size_t)(uint32_t)old_w,
                       (size_t)(uint32_t)cw * 4u);
            }
        }

        if (img && !ptr_is_invalid(img) && img_cap_bytes) {
            munmap((void*)img, img_cap_bytes);
        }
        if (img_shm_fd >= 0) {
            close(img_shm_fd);
        }
        if (img_shm_name[0]) {
            (void)shm_unlink_named(img_shm_name);
        }

        img = nimg;
        img_w = new_w;
        img_h = new_h;
        img_shm_fd = new_fd;
        img_cap_bytes = bytes;
        memcpy(img_shm_name, new_name, sizeof(img_shm_name));
    }

    snapshot_free(&undo_stack[0]);
    snapshot_free(&redo_stack[0]);
    undo_count = 0;
    redo_count = 0;
}

static void img_put_pixel(int x, int y, uint32_t color) {
    size_t count = img_pixel_count();
    if (count == 0) return;
    if ((unsigned)x >= (unsigned)img_w) return;
    if ((unsigned)y >= (unsigned)img_h) return;
    size_t idx = (size_t)(uint32_t)y * (size_t)(uint32_t)img_w + (size_t)(uint32_t)x;
    if (idx >= count) return;
    img[idx] = color;
}

static void img_draw_disc(int cx, int cy, int r, uint32_t color) {
    if (r <= 0) {
        img_put_pixel(cx, cy, color);
        return;
    }
    int rr = r * r;
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            if (dx * dx + dy * dy <= rr) img_put_pixel(cx + dx, cy + dy, color);
        }
    }
}

static void img_draw_line(int x0, int y0, int x1, int y1, int r, uint32_t color) {
    int dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int sx = (x0 < x1) ? 1 : -1;
    int dy = (y1 > y0) ? (y0 - y1) : (y1 - y0);
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx + dy;

    while (1) {
        img_draw_disc(x0, y0, r, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = err * 2;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void img_fill_rect(int x0, int y0, int x1, int y1, uint32_t color) {
    size_t count = img_pixel_count();
    if (count == 0) return;
    size_t w = (size_t)(uint32_t)img_w;
    int lx = min_i(x0, x1);
    int rx = max_i(x0, x1);
    int ty = min_i(y0, y1);
    int by = max_i(y0, y1);

    if (lx < 0) lx = 0;
    if (ty < 0) ty = 0;
    if (rx >= img_w) rx = img_w - 1;
    if (by >= img_h) by = img_h - 1;
    if (lx > rx || ty > by) return;

    for (int y = ty; y <= by; y++) {
        size_t row_off = (size_t)(uint32_t)y * w;
        if (row_off >= count) continue;
        uint32_t* row = img + row_off;
        for (int x = lx; x <= rx; x++) {
            row[x] = color;
        }
    }
}

static void img_draw_rect(int x0, int y0, int x1, int y1, int r, uint32_t color, int fill) {
    int lx = min_i(x0, x1);
    int rx = max_i(x0, x1);
    int ty = min_i(y0, y1);
    int by = max_i(y0, y1);

    if (fill) {
        img_fill_rect(lx, ty, rx, by, color);
    }

    img_draw_line(lx, ty, rx, ty, r, color);
    img_draw_line(lx, by, rx, by, r, color);
    img_draw_line(lx, ty, lx, by, r, color);
    img_draw_line(rx, ty, rx, by, r, color);
}

static void img_fill_circle(int cx, int cy, int rad, uint32_t color) {
    size_t count = img_pixel_count();
    if (count == 0) return;
    size_t w = (size_t)(uint32_t)img_w;
    if (rad <= 0) {
        img_put_pixel(cx, cy, color);
        return;
    }

    int rr = rad * rad;
    for (int yy = -rad; yy <= rad; yy++) {
        int y = cy + yy;
        if ((unsigned)y >= (unsigned)img_h) continue;

        int span = isqrt_i(rr - yy * yy);
        int lx = cx - span;
        int rx = cx + span;
        if (lx < 0) lx = 0;
        if (rx >= img_w) rx = img_w - 1;

        size_t row_off = (size_t)(uint32_t)y * w;
        if (row_off >= count) continue;
        uint32_t* row = img + row_off;
        for (int x = lx; x <= rx; x++) {
            row[x] = color;
        }
    }
}

static void img_draw_circle(int x0, int y0, int x1, int y1, int r, uint32_t color, int fill) {
    int cx = x0;
    int cy = y0;
    int dx = x1 - x0;
    int dy = y1 - y0;
    int rad = isqrt_i(dx * dx + dy * dy);

    if (fill) {
        img_fill_circle(cx, cy, rad, color);
    }

    if (rad <= 0) {
        img_draw_disc(cx, cy, r, color);
        return;
    }

    int x = rad;
    int y = 0;
    int err = 1 - x;
    while (x >= y) {
        img_draw_disc(cx + x, cy + y, r, color);
        img_draw_disc(cx + y, cy + x, r, color);
        img_draw_disc(cx - y, cy + x, r, color);
        img_draw_disc(cx - x, cy + y, r, color);
        img_draw_disc(cx - x, cy - y, r, color);
        img_draw_disc(cx - y, cy - x, r, color);
        img_draw_disc(cx + y, cy - x, r, color);
        img_draw_disc(cx + x, cy - y, r, color);

        y++;
        if (err < 0) {
            err += 2 * y + 1;
        } else {
            x--;
            err += 2 * (y - x + 1);
        }
    }
}

static void flood_fill(int sx, int sy, uint32_t target, uint32_t repl) {
    size_t count = img_pixel_count();
    if (count == 0) return;
    size_t w = (size_t)(uint32_t)img_w;
    if (target == repl) return;
    if ((unsigned)sx >= (unsigned)img_w) return;
    if ((unsigned)sy >= (unsigned)img_h) return;
    {
        size_t start_idx = (size_t)(uint32_t)sy * w + (size_t)(uint32_t)sx;
        if (start_idx >= count) return;
        if (img[start_idx] != target) return;
    }

    typedef struct { int x; int y; } pt_t;
    int cap = 1024;
    int top = 0;
    pt_t* st = (pt_t*)malloc((size_t)cap * sizeof(pt_t));
    if (ptr_is_invalid(st)) return;

    st[top++] = (pt_t){sx, sy};

    while (top > 0) {
        pt_t p = st[--top];
        int x = p.x;
        int y = p.y;

        if ((unsigned)x >= (unsigned)img_w || (unsigned)y >= (unsigned)img_h) continue;
        size_t row_off = (size_t)(uint32_t)y * w;
        if (row_off >= count) continue;
        uint32_t* row = img + row_off;
        if (row[x] != target) continue;

        int lx = x;
        while (lx > 0 && row[lx - 1] == target) lx--;

        int rx = x;
        while (rx + 1 < img_w && row[rx + 1] == target) rx++;

        for (int i = lx; i <= rx; i++) row[i] = repl;

        for (int dir = -1; dir <= 1; dir += 2) {
            int ny = y + dir;
            if ((unsigned)ny >= (unsigned)img_h) continue;

            int i = lx;
            size_t nrow_off = (size_t)(uint32_t)ny * w;
            if (nrow_off >= count) continue;
            uint32_t* nrow = img + nrow_off;
            while (i <= rx) {
                if (nrow[i] == target) {
                    if (top == cap) {
                        int ncap = cap * 2;
                        if (ncap < cap) {
                            free(st);
                            return;
                        }
                        pt_t* nst = (pt_t*)malloc((size_t)ncap * sizeof(pt_t));
                        if (ptr_is_invalid(nst)) {
                            free(st);
                            return;
                        }
                        memcpy(nst, st, (size_t)cap * sizeof(pt_t));
                        free(st);
                        st = nst;
                        cap = ncap;
                    }
                    st[top++] = (pt_t){i, ny};
                    while (i <= rx && nrow[i] == target) i++;
                    continue;
                }
                i++;
            }
        }
    }

    free(st);
}

static void canvas_put_pixel_alpha(int x, int y, uint32_t color, uint8_t alpha) {
    if ((unsigned)x >= (unsigned)WIN_W) return;
    if ((unsigned)y >= (unsigned)WIN_H) return;
    uint32_t* p = &canvas[y * WIN_W + x];
    *p = blend(color, *p, alpha);
}

static void canvas_draw_disc_alpha_img(int cx, int cy, int r, uint32_t color, uint8_t alpha) {
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

static void canvas_draw_line_alpha_img(int x0, int y0, int x1, int y1, int r, uint32_t color, uint8_t alpha) {
    int dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int sx = (x0 < x1) ? 1 : -1;
    int dy = (y1 > y0) ? (y0 - y1) : (y1 - y0);
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx + dy;

    while (1) {
        canvas_draw_disc_alpha_img(x0, y0, r, color, alpha);
        if (x0 == x1 && y0 == y1) break;
        int e2 = err * 2;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void canvas_draw_rect_alpha_img(int x0, int y0, int x1, int y1, int r, uint32_t color, int fill, uint8_t alpha) {
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

static void canvas_draw_circle_alpha_img(int x0, int y0, int x1, int y1, int r, uint32_t color, int fill, uint8_t alpha) {
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

static int tool_name(char* out, int out_cap) {
    const char* s = "Brush";
    if (tool == TOOL_ERASER) s = "Eraser";
    else if (tool == TOOL_LINE) s = "Line";
    else if (tool == TOOL_RECT) s = "Rect";
    else if (tool == TOOL_CIRCLE) s = "Circle";
    else if (tool == TOOL_FILL) s = "Fill";
    else if (tool == TOOL_PICK) s = "Pick";
    return snprintf(out, (size_t)out_cap, "%s", s);
}

static void ui_draw_tool_item(int y, const char* label, int is_active) {
    int x = 8;
    int w = r_toolbar.w - 16;
    int h = 20;

    if (is_active) {
        fill_rect(r_toolbar.x + x, r_toolbar.y + y, w, h, 0x1B1B1C);
        draw_frame(r_toolbar.x + x, r_toolbar.y + y, w, h, C_ACCENT);
    } else {
        fill_rect(r_toolbar.x + x, r_toolbar.y + y, w, h, 0x1E1E1E);
        draw_frame(r_toolbar.x + x, r_toolbar.y + y, w, h, C_BORDER);
    }

    draw_string(canvas, WIN_W, WIN_H, r_toolbar.x + x + 6, r_toolbar.y + y + 6, label, is_active ? C_TEXT : C_TEXT_DIM);
}

static rect_t palette_rect(int idx) {
    int cols = 3;
    int sw = 18;
    int gap = 6;

    int color_bar_y = r_toolbar.y + r_toolbar.h - 56;
    int rows = (8 + cols - 1) / cols;
    int pal_h = rows * sw + (rows - 1) * gap;

    int label_h = 18;
    int pad = 2;

    int py = color_bar_y - (pal_h + label_h + pad);
    int min_py = r_toolbar.y + 220;
    int max_py = color_bar_y - pal_h - pad;

    if (max_py < min_py) {
        py = min_py;
    } else {
        if (py < min_py) py = min_py;
        if (py > max_py) py = max_py;
    }

    int row = idx / cols;
    int col = idx % cols;

    int x = r_toolbar.x + 10 + col * (sw + gap);
    int y = py + row * (sw + gap);
    return (rect_t){x, y, sw, sw};
}

static int palette_hit(int mx, int my) {
    for (int i = 0; i < 8; i++) {
        rect_t pr = palette_rect(i);
        if (pt_in_rect(mx, my, pr)) return i;
    }
    return -1;
}

static void render_all() {
    fill_rect(0, 0, WIN_W, WIN_H, C_WIN_BG);

    fill_rect(r_header.x, r_header.y, r_header.w, r_header.h, C_HEADER_BG);
    draw_frame(r_header.x, r_header.y, r_header.w, r_header.h, 0x000000);
    draw_string(canvas, WIN_W, WIN_H, 10, 12, "Paint", C_TEXT);

    fill_rect(r_toolbar.x, r_toolbar.y, r_toolbar.w, r_toolbar.h, C_PANEL_BG);
    draw_frame(r_toolbar.x, r_toolbar.y, r_toolbar.w, r_toolbar.h, C_BORDER);

    ui_draw_tool_item(10, "Brush (B)", tool == TOOL_BRUSH);
    ui_draw_tool_item(34, "Eraser (E)", tool == TOOL_ERASER);
    ui_draw_tool_item(58, "Line (L)", tool == TOOL_LINE);
    ui_draw_tool_item(82, "Rect (R)", tool == TOOL_RECT);
    ui_draw_tool_item(106, "Circle (C)", tool == TOOL_CIRCLE);
    ui_draw_tool_item(130, "Fill (F)", tool == TOOL_FILL);
    ui_draw_tool_item(154, "Pick (P)", tool == TOOL_PICK);

    int cy = 190;
    draw_string(canvas, WIN_W, WIN_H, r_toolbar.x + 10, r_toolbar.y + cy, "Size:", C_TEXT_DIM);
    char sbuf[32];
    snprintf(sbuf, sizeof(sbuf), "%d", brush_r);
    draw_string(canvas, WIN_W, WIN_H, r_toolbar.x + 54, r_toolbar.y + cy, sbuf, C_TEXT);
    draw_string(canvas, WIN_W, WIN_H, r_toolbar.x + 10, r_toolbar.y + cy + 18, "-/+", C_TEXT_DIM);

    fill_rect(r_status.x, r_status.y, r_status.w, r_status.h, C_HEADER_BG);
    draw_frame(r_status.x, r_status.y, r_status.w, r_status.h, 0x000000);

    char tbuf[64];
    tool_name(tbuf, sizeof(tbuf));
    char st[96];
    snprintf(st, sizeof(st), "Tool: %s  Undo:%d  Redo:%d", tbuf, undo_count, redo_count);
    draw_string(canvas, WIN_W, WIN_H, 8, r_status.y + 7, st, C_TEXT_DIM);

    fill_rect(r_canvas.x, r_canvas.y, r_canvas.w, r_canvas.h, C_CANVAS_BG);
    draw_frame(r_canvas.x, r_canvas.y, r_canvas.w, r_canvas.h, C_BORDER);

    if (img && !ptr_is_invalid(img) && img_w > 0 && img_h > 0) {
        size_t count = img_pixel_count();
        if (count != 0) {
            int cw = min_i(img_w, r_canvas.w);
            int ch = min_i(img_h, r_canvas.h);
            for (int y = 0; y < ch; y++) {
                memcpy(canvas + (r_canvas.y + y) * WIN_W + r_canvas.x, img + y * img_w, (size_t)cw * 4u);
            }
        }
    }

    if (mouse_down && drag_active) {
        uint8_t a = 160;
        if (tool == TOOL_LINE) {
            canvas_draw_line_alpha_img(drag_start_x, drag_start_y, drag_cur_x, drag_cur_y, brush_r, cur_color, a);
        } else if (tool == TOOL_RECT) {
            canvas_draw_rect_alpha_img(drag_start_x, drag_start_y, drag_cur_x, drag_cur_y, brush_r, cur_color, shape_fill, a);
        } else if (tool == TOOL_CIRCLE) {
            canvas_draw_circle_alpha_img(drag_start_x, drag_start_y, drag_cur_x, drag_cur_y, brush_r, cur_color, shape_fill, a);
        }
    }

    if (shape_fill) {
        draw_string(canvas, WIN_W, WIN_H, r_header.w - 90, 12, "FILL", C_ACCENT);
    }

    rect_t p0 = palette_rect(0);
    draw_string(canvas, WIN_W, WIN_H, r_toolbar.x + 10, p0.y - 14, "Colors:", C_TEXT_DIM);
    for (int i = 0; i < 8; i++) {
        rect_t pr = palette_rect(i);
        fill_rect(pr.x, pr.y, pr.w, pr.h, palette[i]);
        draw_frame(pr.x, pr.y, pr.w, pr.h, (palette[i] == cur_color) ? C_ACCENT : 0x000000);
    }

    fill_rect(r_toolbar.x + 10, r_toolbar.y + r_toolbar.h - 56, r_toolbar.w - 20, 18, cur_color);
    draw_frame(r_toolbar.x + 10, r_toolbar.y + r_toolbar.h - 56, r_toolbar.w - 20, 18, 0x000000);
    draw_string(canvas, WIN_W, WIN_H, r_toolbar.x + 10, r_toolbar.y + r_toolbar.h - 32, "Ctrl+Z/Y", C_TEXT_DIM);
}

static int mouse_to_img(int mx, int my, int* out_x, int* out_y) {
    size_t count = img_pixel_count();
    if (count == 0) return 0;
    if (!pt_in_rect(mx, my, r_canvas)) return 0;
    int ix = mx - r_canvas.x;
    int iy = my - r_canvas.y;
    if ((unsigned)ix >= (unsigned)img_w) return 0;
    if ((unsigned)iy >= (unsigned)img_h) return 0;
    *out_x = ix;
    *out_y = iy;
    return 1;
}

static void handle_mouse_down(int mx, int my) {
    g_dbg_stage = 311;
    int ix, iy;
    if (pt_in_rect(mx, my, r_toolbar)) {
        g_dbg_stage = 312;
        int p = palette_hit(mx, my);
        if (p >= 0) {
            cur_color = palette[p];
            return;
        }
        int ry = my - r_toolbar.y;
        if (ry >= 10 && ry < 30) tool = TOOL_BRUSH;
        else if (ry >= 34 && ry < 54) tool = TOOL_ERASER;
        else if (ry >= 58 && ry < 78) tool = TOOL_LINE;
        else if (ry >= 82 && ry < 102) tool = TOOL_RECT;
        else if (ry >= 106 && ry < 126) tool = TOOL_CIRCLE;
        else if (ry >= 130 && ry < 150) tool = TOOL_FILL;
        else if (ry >= 154 && ry < 174) tool = TOOL_PICK;

        mouse_down = 0;
        drag_active = 0;
        return;
    }

    g_dbg_stage = 313;
    if (!mouse_to_img(mx, my, &ix, &iy)) return;

    const size_t img_count = img_pixel_count();
    if (img_count == 0) return;
    const size_t idx = (size_t)(uint32_t)iy * (size_t)(uint32_t)img_w + (size_t)(uint32_t)ix;
    if (idx >= img_count) return;

    if (tool == TOOL_PICK) {
        g_dbg_stage = 314;
        cur_color = img[idx];
        return;
    }

    if (tool == TOOL_FILL) {
        g_dbg_stage = 315;
        push_undo();
        uint32_t target = img[idx];
        flood_fill(ix, iy, target, cur_color);
        return;
    }

    mouse_down = 1;
    last_img_x = ix;
    last_img_y = iy;

    if (tool == TOOL_BRUSH || tool == TOOL_ERASER) {
        g_dbg_stage = 318;
        push_undo();
        uint32_t col = (tool == TOOL_ERASER) ? C_CANVAS_BG : cur_color;
        img_draw_disc(ix, iy, brush_r, col);
        drag_active = 0;
        return;
    }

    if (tool == TOOL_LINE || tool == TOOL_RECT || tool == TOOL_CIRCLE) {
        drag_active = 1;
        drag_start_x = ix;
        drag_start_y = iy;
        drag_cur_x = ix;
        drag_cur_y = iy;
    }
}

static void handle_mouse_move(int mx, int my) {
    if (!mouse_down) return;

    int ix, iy;
    if (!mouse_to_img(mx, my, &ix, &iy)) return;

    if (tool == TOOL_BRUSH || tool == TOOL_ERASER) {
        uint32_t col = (tool == TOOL_ERASER) ? C_CANVAS_BG : cur_color;
        img_draw_line(last_img_x, last_img_y, ix, iy, brush_r, col);
        last_img_x = ix;
        last_img_y = iy;
    } else if (drag_active) {
        drag_cur_x = ix;
        drag_cur_y = iy;
    }
}

static void handle_mouse_up() {
    if (mouse_down && drag_active) {
        push_undo();
        if (tool == TOOL_LINE) {
            img_draw_line(drag_start_x, drag_start_y, drag_cur_x, drag_cur_y, brush_r, cur_color);
        } else if (tool == TOOL_RECT) {
            img_draw_rect(drag_start_x, drag_start_y, drag_cur_x, drag_cur_y, brush_r, cur_color, shape_fill);
        } else if (tool == TOOL_CIRCLE) {
            img_draw_circle(drag_start_x, drag_start_y, drag_cur_x, drag_cur_y, brush_r, cur_color, shape_fill);
        }
    }

    mouse_down = 0;
    drag_active = 0;
}

static int handle_key(unsigned char c) {
    if (c == 'q' || c == 'Q' || c == 0x17) {
        return 1;
    }

    if (c == 'b' || c == 'B') tool = TOOL_BRUSH;
    else if (c == 'e' || c == 'E') tool = TOOL_ERASER;
    else if (c == 'l' || c == 'L') tool = TOOL_LINE;
    else if (c == 'r' || c == 'R') tool = TOOL_RECT;
    else if (c == 'c' || c == 'C') tool = TOOL_CIRCLE;
    else if (c == 'f' || c == 'F') tool = TOOL_FILL;
    else if (c == 'p' || c == 'P') tool = TOOL_PICK;

    else if (c == '+' || c == '=') { if (brush_r < 32) brush_r++; }
    else if (c == '-' || c == '_') { if (brush_r > 0) brush_r--; }

    else if (c == 'g' || c == 'G') shape_fill = !shape_fill;

    else if (c == 0x1A) do_undo();
    else if (c == 0x19) do_redo();

    else if (c >= '1' && c <= '8') {
        cur_color = palette[(int)(c - '1')];
    }

    return 0;
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    dbg_write("paint: start\n");

    set_term_mode(0);

    g_dbg_stage = 1;

    (void)signal(SIGSEGV, (void*)paint_on_signal);
    (void)signal(SIGILL, (void*)paint_on_signal);
    (void)signal(SIGTERM, (void*)paint_on_signal);
    (void)signal(SIGINT, (void*)paint_on_signal);

    const uint32_t surface_id = 1u;
    uint32_t size_bytes = (uint32_t)WIN_W * (uint32_t)WIN_H * 4u;

    comp_conn_t conn;
    comp_conn_reset(&conn);
    if (comp_connect(&conn, "compositor") != 0) {
        dbg_write("paint: comp_connect failed\n");
        return 1;
    }
    if (comp_send_hello(&conn) != 0) {
        dbg_write("paint: hello failed\n");
        comp_disconnect(&conn);
        return 1;
    }

    char shm_name[32];
    shm_name[0] = '\0';

    const int pid = getpid();
    int shm_fd = -1;
    int shm_gen = 0;
    for (int i = 0; i < 8; i++) {
        (void)snprintf(shm_name, sizeof(shm_name), "paint_%d_%d", pid, i);
        shm_fd = shm_create_named(shm_name, size_bytes);
        if (shm_fd >= 0) break;
    }
    if (shm_fd < 0) {
        dbg_write("paint: shm_create_named failed\n");
        comp_disconnect(&conn);
        return 1;
    }

    canvas = (uint32_t*)mmap(shm_fd, size_bytes, MAP_SHARED);
    if (ptr_is_invalid(canvas)) {
        dbg_write("paint: mmap(shm) failed\n");
        close(shm_fd);
        shm_unlink_named(shm_name);
        comp_disconnect(&conn);
        return 1;
    }

    layout_update();
    img_resize_to_canvas();

    render_all();

    if (comp_send_attach_shm_name(&conn, surface_id, shm_name, size_bytes, (uint32_t)WIN_W, (uint32_t)WIN_H, (uint32_t)WIN_W, 0u) != 0) {
        dbg_write("paint: attach_shm_name failed\n");
        munmap((void*)canvas, size_bytes);
        canvas = 0;
        close(shm_fd);
        shm_unlink_named(shm_name);
        comp_disconnect(&conn);
        return 1;
    }
    if (comp_send_commit(&conn, surface_id, 32, 32, 0u) != 0) {
        dbg_write("paint: commit failed\n");
        (void)comp_send_destroy_surface(&conn, surface_id, 0u);
        munmap((void*)canvas, size_bytes);
        canvas = 0;
        close(shm_fd);
        shm_unlink_named(shm_name);
        comp_disconnect(&conn);
        return 1;
    }

    dbg_write("paint: committed\n");

    int running = 1;
    int last_buttons = 0;
    int last_mx = 0;
    int last_my = 0;
    int have_mouse = 0;

    comp_ipc_hdr_t hdr;
    uint8_t payload[COMP_IPC_MAX_PAYLOAD];

    while (running) {
        int need_update = 0;

        for (;;) {
            int rr = comp_try_recv(&conn, &hdr, payload, (uint32_t)sizeof(payload));
            if (rr < 0) {
                running = 0;
                break;
            }
            if (rr == 0) break;

            if (hdr.type != (uint16_t)COMP_IPC_MSG_INPUT || hdr.len != (uint32_t)sizeof(comp_ipc_input_t)) {
                continue;
            }

            comp_ipc_input_t in;
            memcpy(&in, payload, sizeof(in));
            if (in.surface_id != surface_id) continue;

            if (in.kind == COMP_IPC_INPUT_RESIZE) {
                g_dbg_stage = 100;
                int32_t nw_i = in.x;
                int32_t nh_i = in.y;
                g_dbg_resize_w = nw_i;
                g_dbg_resize_h = nh_i;
                if (nw_i <= 0 || nh_i <= 0) continue;
                if (nw_i == WIN_W && nh_i == WIN_H) continue;

                const uint32_t max_pix = (uint32_t)PAINT_MAX_SURFACE_PIXELS;
                if (max_pix == 0) continue;
                {
                    uint64_t want_pix64 = (uint64_t)(uint32_t)nw_i * (uint64_t)(uint32_t)nh_i;
                    if (want_pix64 > (uint64_t)max_pix) {
                        if (nw_i >= nh_i) {
                            nw_i = (int32_t)((uint32_t)max_pix / (uint32_t)nh_i);
                        } else {
                            nh_i = (int32_t)((uint32_t)max_pix / (uint32_t)nw_i);
                        }
                        if (nw_i <= 0 || nh_i <= 0) continue;
                    }
                }

                uint64_t bytes64 = (uint64_t)(uint32_t)nw_i * (uint64_t)(uint32_t)nh_i * 4u;
                if (bytes64 == 0 || bytes64 > 0xFFFFFFFFu || bytes64 > (uint64_t)PAINT_MAX_SURFACE_BYTES) continue;
                uint32_t need_size_bytes = (uint32_t)bytes64;

                const int can_reuse_shm = (need_size_bytes <= size_bytes) && (shm_name[0] != '\0') && (shm_fd >= 0) && canvas;
                if (can_reuse_shm) {
                    g_dbg_stage = 110;
                    const int old_w = WIN_W;
                    const int old_h = WIN_H;
                    uint16_t err_code = 0;
                    if (comp_send_attach_shm_name_sync(&conn, surface_id, shm_name, size_bytes, (uint32_t)nw_i, (uint32_t)nh_i, (uint32_t)nw_i, 0u, 2000u, &err_code) != 0) {
                        char tmp[96];
                        (void)snprintf(tmp, sizeof(tmp), "paint: resize attach failed err=%u\n", (unsigned)err_code);
                        dbg_write(tmp);
                        continue;
                    }

                    g_dbg_stage = 120;
                    WIN_W = nw_i;
                    WIN_H = nh_i;
                    layout_update();
                    g_dbg_stage = 121;
                    img_resize_to_canvas();
                    g_dbg_stage = 122;
                    render_all();

                    g_dbg_stage = 130;
                    if (comp_send_commit_sync(&conn, surface_id, 32, 32, 0u, 2000u, &err_code) != 0) {
                        char tmp[96];
                        (void)snprintf(tmp, sizeof(tmp), "paint: resize commit failed err=%u\n", (unsigned)err_code);
                        dbg_write(tmp);

                        WIN_W = old_w;
                        WIN_H = old_h;
                        layout_update();
                        img_resize_to_canvas();
                        render_all();
                        continue;
                    }

                    mouse_down = 0;
                    drag_active = 0;
                    have_mouse = 0;
                    last_buttons = 0;
                    need_update = 0;
                    continue;
                }

                uint64_t grow64 = (uint64_t)size_bytes * 2ull;
                uint64_t new_cap64 = (grow64 >= (uint64_t)need_size_bytes) ? grow64 : (uint64_t)need_size_bytes;
                if (new_cap64 > (uint64_t)PAINT_MAX_SURFACE_BYTES) new_cap64 = (uint64_t)need_size_bytes;
                if (new_cap64 > 0xFFFFFFFFu) continue;
                uint32_t new_cap_bytes = (uint32_t)new_cap64;

                char new_shm_name[32];
                new_shm_name[0] = '\0';
                int new_shm_fd = -1;
                g_dbg_stage = 200;
                for (int i = 0; i < 16; i++) {
                    shm_gen++;
                    (void)snprintf(new_shm_name, sizeof(new_shm_name), "paint_%d_r%d", pid, shm_gen);
                    new_shm_fd = shm_create_named(new_shm_name, new_cap_bytes);
                    if (new_shm_fd >= 0) break;
                }
                if (new_shm_fd < 0) continue;

                g_dbg_stage = 210;
                uint32_t* new_canvas = (uint32_t*)mmap(new_shm_fd, new_cap_bytes, MAP_SHARED);
                if (ptr_is_invalid(new_canvas)) {
                    dbg_write("paint: resize mmap failed\n");
                    close(new_shm_fd);
                    shm_unlink_named(new_shm_name);
                    continue;
                }

                uint16_t err_code = 0;
                g_dbg_stage = 220;
                if (comp_send_attach_shm_name_sync(&conn, surface_id, new_shm_name, new_cap_bytes, (uint32_t)nw_i, (uint32_t)nh_i, (uint32_t)nw_i, 0u, 2000u, &err_code) != 0) {
                    char tmp[96];
                    (void)snprintf(tmp, sizeof(tmp), "paint: resize attach(new) failed err=%u\n", (unsigned)err_code);
                    dbg_write(tmp);
                    munmap((void*)new_canvas, new_cap_bytes);
                    close(new_shm_fd);
                    shm_unlink_named(new_shm_name);
                    continue;
                }

                uint32_t* old_canvas = canvas;
                uint32_t old_size_bytes = size_bytes;
                int old_shm_fd = shm_fd;
                int old_w = WIN_W;
                int old_h = WIN_H;
                char old_shm_name[32];
                memcpy(old_shm_name, shm_name, sizeof(old_shm_name));

                canvas = new_canvas;
                WIN_W = nw_i;
                WIN_H = nh_i;
                size_bytes = new_cap_bytes;
                shm_fd = new_shm_fd;
                memcpy(shm_name, new_shm_name, sizeof(shm_name));

                g_dbg_stage = 230;
                layout_update();
                g_dbg_stage = 231;
                img_resize_to_canvas();
                g_dbg_stage = 232;
                render_all();

                g_dbg_stage = 240;
                if (comp_send_commit_sync(&conn, surface_id, 32, 32, 0u, 2000u, &err_code) != 0) {
                    char tmp[96];
                    (void)snprintf(tmp, sizeof(tmp), "paint: resize commit(new) failed err=%u\n", (unsigned)err_code);
                    dbg_write(tmp);

                    canvas = old_canvas;
                    WIN_W = old_w;
                    WIN_H = old_h;
                    size_bytes = old_size_bytes;
                    shm_fd = old_shm_fd;
                    memcpy(shm_name, old_shm_name, sizeof(shm_name));
                    layout_update();
                    img_resize_to_canvas();
                    render_all();

                    munmap((void*)new_canvas, new_cap_bytes);
                    close(new_shm_fd);
                    shm_unlink_named(new_shm_name);
                    need_update = 1;
                    continue;
                }

                if (old_canvas) {
                    munmap((void*)old_canvas, old_size_bytes);
                }
                if (old_shm_fd >= 0) {
                    close(old_shm_fd);
                }
                if (old_shm_name[0]) {
                    shm_unlink_named(old_shm_name);
                }

                mouse_down = 0;
                drag_active = 0;
                have_mouse = 0;
                last_buttons = 0;
                need_update = 0;
                continue;
            }

            if (in.kind == COMP_IPC_INPUT_KEY) {
                if (in.key_state == 1u) {
                    if (handle_key((unsigned char)(uint8_t)in.keycode)) {
                        running = 0;
                        break;
                    }
                    need_update = 1;
                }
                continue;
            }

            if (in.kind == COMP_IPC_INPUT_MOUSE) {
                const int mx = (int)in.x;
                const int my = (int)in.y;
                const int buttons = (int)in.buttons;

                const int prev_buttons = have_mouse ? last_buttons : 0;
                if (!have_mouse) {
                    last_mx = mx;
                    last_my = my;
                    have_mouse = 1;
                }

                const int down_now = (buttons & 1) != 0;
                const int down_prev = (prev_buttons & 1) != 0;

                if (down_now && !down_prev) {
                    g_dbg_stage = 310;
                    handle_mouse_down(mx, my);
                    need_update = 1;
                }
                if (!down_now && down_prev) {
                    g_dbg_stage = 330;
                    handle_mouse_up();
                    need_update = 1;
                }
                if (mx != last_mx || my != last_my) {
                    if (mouse_down) {
                        g_dbg_stage = 320;
                        handle_mouse_move(mx, my);
                        need_update = 1;
                    }
                }

                last_mx = mx;
                last_my = my;
                last_buttons = buttons;
            }
        }

        if (need_update && canvas) {
            g_dbg_stage = 400;
            render_all();
            g_dbg_stage = 401;
            if (comp_send_commit(&conn, surface_id, 32, 32, 0u) != 0) {
                dbg_write("paint: commit failed\n");
                running = 0;
                break;
            }
        }
        comp_wait_events(&conn, 16000u);
    }

    for (int i = 0; i < undo_count; i++) snapshot_free(&undo_stack[i]);
    for (int i = 0; i < redo_count; i++) snapshot_free(&redo_stack[i]);
    if (img && !ptr_is_invalid(img) && img_cap_bytes) {
        munmap((void*)img, img_cap_bytes);
    }
    img = 0;
    if (img_shm_fd >= 0) {
        close(img_shm_fd);
        img_shm_fd = -1;
    }
    if (img_shm_name[0]) {
        (void)shm_unlink_named(img_shm_name);
        img_shm_name[0] = '\0';
    }
    img_cap_bytes = 0;

    (void)comp_send_destroy_surface(&conn, surface_id, 0u);
    comp_disconnect(&conn);

    if (canvas) {
        munmap((void*)canvas, size_bytes);
        canvas = 0;
    }
    if (shm_fd >= 0) {
        close(shm_fd);
        shm_fd = -1;
    }
    if (shm_name[0]) {
        shm_unlink_named(shm_name);
        shm_name[0] = '\0';
    }

    return 0;
}
