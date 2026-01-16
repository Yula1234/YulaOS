// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#include <yula.h>
#include <font.h>

static int WIN_W = 800;
static int WIN_H = 600;

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
static int win_id;

static rect_t r_header;
static rect_t r_toolbar;
static rect_t r_status;
static rect_t r_canvas;

static uint32_t* img;
static int img_w;
static int img_h;

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
    if (s->pixels && s->w == w && s->h == h) return 1;

    size_t bytes = (size_t)w * (size_t)h * 4u;
    if (bytes == 0) return 0;

    void* p = s->pixels ? realloc(s->pixels, bytes) : malloc(bytes);
    if (!p) return 0;

    s->pixels = (uint32_t*)p;
    s->w = w;
    s->h = h;
    return 1;
}

static void snapshot_free(snapshot_t* s) {
    if (!s) return;
    if (s->pixels) free(s->pixels);
    s->pixels = 0;
    s->w = 0;
    s->h = 0;
}

static int snapshot_capture(snapshot_t* out) {
    if (!out || !img || img_w <= 0 || img_h <= 0) return 0;
    if (!snapshot_reserve(out, img_w, img_h)) return 0;
    size_t bytes = (size_t)img_w * (size_t)img_h * 4u;
    memcpy(out->pixels, img, bytes);
    return 1;
}

static void img_swap_with_snapshot(snapshot_t* s) {
    if (!s || !img) return;
    if (!s->pixels) return;
    if (s->w != img_w || s->h != img_h) return;

    size_t count = (size_t)img_w * (size_t)img_h;
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
    if (!snapshot_capture(&undo_stack[0])) return;
    undo_count = 1;
    clear_redo();
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

    if (img && img_w == new_w && img_h == new_h) return;

    size_t bytes = (size_t)new_w * (size_t)new_h * 4u;
    uint32_t* nimg = (uint32_t*)malloc(bytes);
    if (!nimg) return;

    for (int i = 0; i < new_w * new_h; i++) nimg[i] = C_CANVAS_BG;

    if (img) {
        int cw = min_i(img_w, new_w);
        int ch = min_i(img_h, new_h);
        for (int y = 0; y < ch; y++) {
            memcpy(nimg + y * new_w, img + y * img_w, (size_t)cw * 4u);
        }
        free(img);
    }

    img = nimg;
    img_w = new_w;
    img_h = new_h;

    for (int i = 0; i < undo_count; i++) snapshot_free(&undo_stack[i]);
    for (int i = 0; i < redo_count; i++) snapshot_free(&redo_stack[i]);
    undo_count = 0;
    redo_count = 0;
}

static void img_put_pixel(int x, int y, uint32_t color) {
    if ((unsigned)x >= (unsigned)img_w) return;
    if ((unsigned)y >= (unsigned)img_h) return;
    img[y * img_w + x] = color;
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
        uint32_t* row = img + y * img_w;
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

        uint32_t* row = img + y * img_w;
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
    if (!img) return;
    if (target == repl) return;
    if ((unsigned)sx >= (unsigned)img_w) return;
    if ((unsigned)sy >= (unsigned)img_h) return;
    if (img[sy * img_w + sx] != target) return;

    typedef struct { int x; int y; } pt_t;
    int cap = 1024;
    int top = 0;
    pt_t* st = (pt_t*)malloc((size_t)cap * sizeof(pt_t));
    if (!st) return;

    st[top++] = (pt_t){sx, sy};

    while (top > 0) {
        pt_t p = st[--top];
        int x = p.x;
        int y = p.y;

        if ((unsigned)x >= (unsigned)img_w || (unsigned)y >= (unsigned)img_h) continue;
        if (img[y * img_w + x] != target) continue;

        int lx = x;
        while (lx > 0 && img[y * img_w + (lx - 1)] == target) lx--;

        int rx = x;
        while (rx + 1 < img_w && img[y * img_w + (rx + 1)] == target) rx++;

        uint32_t* row = img + y * img_w;
        for (int i = lx; i <= rx; i++) row[i] = repl;

        for (int dir = -1; dir <= 1; dir += 2) {
            int ny = y + dir;
            if ((unsigned)ny >= (unsigned)img_h) continue;

            int i = lx;
            uint32_t* nrow = img + ny * img_w;
            while (i <= rx) {
                if (nrow[i] == target) {
                    if (top == cap) {
                        int ncap = cap * 2;
                        pt_t* nst = (pt_t*)realloc(st, (size_t)ncap * sizeof(pt_t));
                        if (!nst) {
                            free(st);
                            return;
                        }
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

    if (img) {
        int cw = min_i(img_w, r_canvas.w);
        int ch = min_i(img_h, r_canvas.h);
        for (int y = 0; y < ch; y++) {
            memcpy(canvas + (r_canvas.y + y) * WIN_W + r_canvas.x, img + y * img_w, (size_t)cw * 4u);
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
    int ix, iy;
    if (pt_in_rect(mx, my, r_toolbar)) {
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

    if (!mouse_to_img(mx, my, &ix, &iy)) return;

    if (tool == TOOL_PICK) {
        cur_color = img[iy * img_w + ix];
        return;
    }

    if (tool == TOOL_FILL) {
        push_undo();
        uint32_t target = img[iy * img_w + ix];
        flood_fill(ix, iy, target, cur_color);
        return;
    }

    mouse_down = 1;
    last_img_x = ix;
    last_img_y = iy;

    if (tool == TOOL_BRUSH || tool == TOOL_ERASER) {
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

    win_id = create_window(WIN_W, WIN_H, "Paint");
    if (win_id < 0) return 1;

    canvas = (uint32_t*)map_window(win_id);
    if (!canvas) return 1;

    layout_update();
    img_resize_to_canvas();

    render_all();
    update_window(win_id);

    yula_event_t ev;
    int running = 1;

    while (running) {
        int need_update = 0;

        while (get_event(win_id, &ev)) {
            if (ev.type == YULA_EVENT_KEY_DOWN) {
                if (handle_key((unsigned char)ev.arg1)) {
                    running = 0;
                    break;
                }
                need_update = 1;
            } else if (ev.type == YULA_EVENT_MOUSE_DOWN) {
                handle_mouse_down(ev.arg1, ev.arg2);
                need_update = 1;
            } else if (ev.type == YULA_EVENT_MOUSE_MOVE) {
                handle_mouse_move(ev.arg1, ev.arg2);
                need_update = 1;
            } else if (ev.type == YULA_EVENT_MOUSE_UP) {
                handle_mouse_up();
                need_update = 1;
            } else if (ev.type == YULA_EVENT_RESIZE) {
                WIN_W = ev.arg1;
                WIN_H = ev.arg2;
                canvas = (uint32_t*)map_window(win_id);
                if (!canvas) { running = 0; break; }
                layout_update();
                img_resize_to_canvas();

                mouse_down = 0;
                drag_active = 0;

                need_update = 1;
            }
        }

        if (need_update && canvas) {
            render_all();
            update_window(win_id);
        }
        usleep(8000);
    }

    for (int i = 0; i < undo_count; i++) snapshot_free(&undo_stack[i]);
    for (int i = 0; i < redo_count; i++) snapshot_free(&redo_stack[i]);
    if (img) free(img);

    return 0;
}
