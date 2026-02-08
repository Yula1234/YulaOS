// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#include "paint_image.h"

size_t img_pixel_count(void) {
    if (ptr_is_invalid(img)) {
        return 0;
    }
    if (img_w <= 0 || img_h <= 0) {
        return 0;
    }
    size_t c = (size_t)(uint32_t)img_w * (size_t)(uint32_t)img_h;
    if (c == 0) {
        return 0;
    }
    if (c > (size_t)PAINT_MAX_IMG_PIXELS) {
        return 0;
    }
    return c;
}

void snapshot_init(snapshot_t* s, char tag) {
    if (!s) {
        return;
    }
    memset(s, 0, sizeof(*s));
    s->shm_fd = -1;
    s->tag = tag;
}

void snapshot_free(snapshot_t* s) {
    if (!s) {
        return;
    }
    if (s->pixels && !ptr_is_invalid(s->pixels) && s->cap_bytes) {
        munmap((void*)s->pixels, s->cap_bytes);
    }
    if (s->shm_fd >= 0) {
        close(s->shm_fd);
    }
    if (s->shm_name[0]) {
        (void)shm_unlink_named(s->shm_name);
    }
    s->pixels = 0;
    s->w = 0;
    s->h = 0;
    s->cap_bytes = 0;
    s->shm_fd = -1;
    s->shm_name[0] = '\0';
}

static int snapshot_reserve(snapshot_t* s, int w, int h) {
    if (!s) {
        return 0;
    }
    if (w <= 0 || h <= 0) {
        return 0;
    }
    if (s->pixels && !ptr_is_invalid(s->pixels) && s->w == w && s->h == h) {
        return 1;
    }
    uint64_t bytes64 = (uint64_t)(uint32_t)w * (uint64_t)(uint32_t)h * 4ull;
    if (bytes64 == 0 || bytes64 > (uint64_t)PAINT_MAX_IMG_BYTES) {
        return 0;
    }
    if (bytes64 > (uint64_t)(size_t)-1) {
        return 0;
    }
    if (s->pixels && !ptr_is_invalid(s->pixels) && s->cap_bytes >= (uint32_t)bytes64) {
        s->w = w;
        s->h = h;
        return 1;
    }
    char name[32];
    name[0] = '\0';
    const int pid = getpid();
    int fd = -1;
    for (int i = 0; i < 16; i++) {
        s->shm_gen++;
        (void)snprintf(
            name,
            sizeof(name),
            "paintsnap_%d_%c_%u",
            pid,
            s->tag ? s->tag : 's',
            (unsigned)s->shm_gen
        );
        fd = shm_create_named(name, (uint32_t)bytes64);
        if (fd >= 0) {
            break;
        }
    }
    if (fd < 0) {
        return 0;
    }
    g_dbg_stage = 3165;
    uint32_t* px = (uint32_t*)mmap(fd, (uint32_t)bytes64, MAP_SHARED);
    if (ptr_is_invalid(px)) {
        close(fd);
        shm_unlink_named(name);
        return 0;
    }
    g_dbg_stage = 3166;
    snapshot_free(s);
    s->pixels = px;
    s->w = w;
    s->h = h;
    s->shm_fd = fd;
    s->cap_bytes = (uint32_t)bytes64;
    {
        size_t n = strlen(name);
        if (n >= sizeof(s->shm_name)) {
            n = sizeof(s->shm_name) - 1u;
        }
        memcpy(s->shm_name, name, n);
        s->shm_name[n] = '\0';
    }
    return 1;
}

static int snapshot_capture(snapshot_t* out) {
    g_dbg_stage = 3161;
    size_t count = img_pixel_count();
    if (!out || count == 0) {
        return 0;
    }
    g_dbg_stage = 3162;
    if (!snapshot_reserve(out, img_w, img_h)) {
        return 0;
    }
    size_t bytes = count * 4u;
    if (bytes == 0 || bytes > (size_t)PAINT_MAX_IMG_BYTES) {
        return 0;
    }
    g_dbg_stage = 3167;
    memcpy(out->pixels, img, bytes);
    g_dbg_stage = 3168;
    return 1;
}

static void img_swap_with_snapshot(snapshot_t* s) {
    if (!s || ptr_is_invalid(img)) {
        return;
    }
    if (ptr_is_invalid(s->pixels)) {
        return;
    }
    if (s->w != img_w || s->h != img_h) {
        return;
    }
    size_t count = img_pixel_count();
    if (count == 0) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        uint32_t t = img[i];
        img[i] = s->pixels[i];
        s->pixels[i] = t;
    }
}

static void clear_redo(void) {
    redo_count = 0;
}

void push_undo(void) {
    g_dbg_stage = 316;
    g_dbg_stage = 3160;
    if (!snapshot_capture(&undo_stack[0])) {
        return;
    }
    undo_count = 1;
    clear_redo();
    g_dbg_stage = 317;
}

void do_undo(void) {
    if (undo_count <= 0 || !img) {
        return;
    }
    img_swap_with_snapshot(&undo_stack[0]);
    redo_count = 1;
    undo_count = 0;
}

void do_redo(void) {
    if (redo_count <= 0 || !img) {
        return;
    }
    img_swap_with_snapshot(&undo_stack[0]);
    undo_count = 1;
    redo_count = 0;
}

void img_resize_to_canvas(void) {
    int new_w = r_canvas.w;
    int new_h = r_canvas.h;
    if (new_w <= 0 || new_h <= 0) {
        return;
    }
    if (!ptr_is_invalid(img) && img && img_w == new_w && img_h == new_h) {
        return;
    }
    const uint32_t max_img_pixels = (uint32_t)PAINT_MAX_IMG_PIXELS;
    if (max_img_pixels == 0) {
        return;
    }
    uint64_t want_pixels64 = (uint64_t)(uint32_t)new_w * (uint64_t)(uint32_t)new_h;
    if (want_pixels64 > (uint64_t)max_img_pixels) {
        if (new_w >= new_h) {
            new_w = (int)((uint32_t)max_img_pixels / (uint32_t)new_h);
        } else {
            new_h = (int)((uint32_t)max_img_pixels / (uint32_t)new_w);
        }
        if (new_w <= 0 || new_h <= 0) {
            return;
        }
    }
    uint64_t bytes64 = (uint64_t)(uint32_t)new_w * (uint64_t)(uint32_t)new_h * 4ull;
    if (bytes64 == 0 || bytes64 > 0xFFFFFFFFu) {
        return;
    }
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
        if (bytes > (uint32_t)PAINT_MAX_IMG_BYTES) {
            return;
        }
        char new_name[32];
        new_name[0] = '\0';
        int new_fd = -1;
        const int pid = getpid();
        for (int i = 0; i < 16; i++) {
            img_shm_gen++;
            (void)snprintf(new_name, sizeof(new_name), "paintimg_%d_%d", pid, img_shm_gen);
            new_fd = shm_create_named(new_name, bytes);
            if (new_fd >= 0) {
                break;
            }
        }
        if (new_fd < 0) {
            return;
        }
        uint32_t* nimg = (uint32_t*)mmap(new_fd, bytes, MAP_SHARED);
        if (ptr_is_invalid(nimg)) {
            close(new_fd);
            shm_unlink_named(new_name);
            return;
        }
        size_t count = (size_t)(uint32_t)new_w * (size_t)(uint32_t)new_h;
        for (size_t i = 0; i < count; i++) {
            nimg[i] = C_CANVAS_BG;
        }
        if (img && !ptr_is_invalid(img) && old_w > 0 && old_h > 0) {
            int cw = min_i(old_w, new_w);
            int ch = min_i(old_h, new_h);
            for (int y = 0; y < ch; y++) {
                memcpy(
                    nimg + (size_t)(uint32_t)y * (size_t)(uint32_t)new_w,
                    img + (size_t)(uint32_t)y * (size_t)(uint32_t)old_w,
                    (size_t)(uint32_t)cw * 4u
                );
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

void img_put_pixel(int x, int y, uint32_t color) {
    size_t count = img_pixel_count();
    if (count == 0) {
        return;
    }
    if ((unsigned)x >= (unsigned)img_w) {
        return;
    }
    if ((unsigned)y >= (unsigned)img_h) {
        return;
    }
    size_t idx = (size_t)(uint32_t)y * (size_t)(uint32_t)img_w + (size_t)(uint32_t)x;
    if (idx >= count) {
        return;
    }
    img[idx] = color;
}

void img_draw_disc(int cx, int cy, int r, uint32_t color) {
    if (r <= 0) {
        img_put_pixel(cx, cy, color);
        return;
    }
    int rr = r * r;
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            if (dx * dx + dy * dy <= rr) {
                img_put_pixel(cx + dx, cy + dy, color);
            }
        }
    }
}

void img_draw_line(int x0, int y0, int x1, int y1, int r, uint32_t color) {
    int dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int sx = (x0 < x1) ? 1 : -1;
    int dy = (y1 > y0) ? (y0 - y1) : (y1 - y0);
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx + dy;
    while (1) {
        img_draw_disc(x0, y0, r, color);
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

void img_fill_rect(int x0, int y0, int x1, int y1, uint32_t color) {
    size_t count = img_pixel_count();
    if (count == 0) {
        return;
    }
    size_t w = (size_t)(uint32_t)img_w;
    int lx = min_i(x0, x1);
    int rx = max_i(x0, x1);
    int ty = min_i(y0, y1);
    int by = max_i(y0, y1);
    if (lx < 0) {
        lx = 0;
    }
    if (ty < 0) {
        ty = 0;
    }
    if (rx >= img_w) {
        rx = img_w - 1;
    }
    if (by >= img_h) {
        by = img_h - 1;
    }
    if (lx > rx || ty > by) {
        return;
    }
    for (int y = ty; y <= by; y++) {
        size_t row_off = (size_t)(uint32_t)y * w;
        if (row_off >= count) {
            continue;
        }
        uint32_t* row = img + row_off;
        for (int x = lx; x <= rx; x++) {
            row[x] = color;
        }
    }
}

void img_draw_rect(int x0, int y0, int x1, int y1, int r, uint32_t color, int fill) {
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

void img_fill_circle(int cx, int cy, int rad, uint32_t color) {
    size_t count = img_pixel_count();
    if (count == 0) {
        return;
    }
    size_t w = (size_t)(uint32_t)img_w;
    if (rad <= 0) {
        img_put_pixel(cx, cy, color);
        return;
    }
    int rr = rad * rad;
    for (int yy = -rad; yy <= rad; yy++) {
        int y = cy + yy;
        if ((unsigned)y >= (unsigned)img_h) {
            continue;
        }
        int span = isqrt_i(rr - yy * yy);
        int lx = cx - span;
        int rx = cx + span;
        if (lx < 0) {
            lx = 0;
        }
        if (rx >= img_w) {
            rx = img_w - 1;
        }
        size_t row_off = (size_t)(uint32_t)y * w;
        if (row_off >= count) {
            continue;
        }
        uint32_t* row = img + row_off;
        for (int x = lx; x <= rx; x++) {
            row[x] = color;
        }
    }
}

void img_draw_circle(int x0, int y0, int x1, int y1, int r, uint32_t color, int fill) {
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

void flood_fill(int sx, int sy, uint32_t target, uint32_t repl) {
    size_t count = img_pixel_count();
    if (count == 0) {
        return;
    }
    size_t w = (size_t)(uint32_t)img_w;
    if (target == repl) {
        return;
    }
    if ((unsigned)sx >= (unsigned)img_w) {
        return;
    }
    if ((unsigned)sy >= (unsigned)img_h) {
        return;
    }
    {
        size_t start_idx = (size_t)(uint32_t)sy * w + (size_t)(uint32_t)sx;
        if (start_idx >= count) {
            return;
        }
        if (img[start_idx] != target) {
            return;
        }
    }
    typedef struct {
        int x;
        int y;
    } pt_t;
    int cap = 1024;
    int top = 0;
    pt_t* st = (pt_t*)malloc((size_t)cap * sizeof(pt_t));
    if (ptr_is_invalid(st)) {
        return;
    }
    st[top].x = sx;
    st[top].y = sy;
    top++;
    while (top > 0) {
        top--;
        pt_t p = st[top];
        int x = p.x;
        int y = p.y;
        if ((unsigned)x >= (unsigned)img_w || (unsigned)y >= (unsigned)img_h) {
            continue;
        }
        size_t row_off = (size_t)(uint32_t)y * w;
        if (row_off >= count) {
            continue;
        }
        uint32_t* row = img + row_off;
        if (row[x] != target) {
            continue;
        }
        int lx = x;
        while (lx > 0 && row[lx - 1] == target) {
            lx--;
        }
        int rx = x;
        while (rx + 1 < img_w && row[rx + 1] == target) {
            rx++;
        }
        for (int i = lx; i <= rx; i++) {
            row[i] = repl;
        }
        for (int dir = -1; dir <= 1; dir += 2) {
            int ny = y + dir;
            if ((unsigned)ny >= (unsigned)img_h) {
                continue;
            }
            int i = lx;
            size_t nrow_off = (size_t)(uint32_t)ny * w;
            if (nrow_off >= count) {
                continue;
            }
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
                    st[top].x = i;
                    st[top].y = ny;
                    top++;
                    while (i <= rx && nrow[i] == target) {
                        i++;
                    }
                    continue;
                }
                i++;
            }
        }
    }
    free(st);
}
