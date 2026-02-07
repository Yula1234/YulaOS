#include <yula.h>
#include <comp.h>
#include <font.h>

static int WIN_W = 800;
static int WIN_H = 600;

static const int TERM_SCALE_MIN = 1;
static const int TERM_SCALE_MAX = 4;
static const int TERM_SCALE_DEFAULT = 1;
static int TERM_SCALE = TERM_SCALE_DEFAULT;

static const int TERM_PAD_X = 8;
static const int TERM_PAD_Y = 8;

static const uint32_t surface_id = 1u;

static comp_conn_t conn;
static char shm_name[32];
static int shm_fd = -1;
static int shm_gen = 0;
static uint32_t size_bytes = 0;
static uint32_t* canvas = 0;

static int ensure_surface(uint32_t need_w, uint32_t need_h) {
    if (need_w == 0 || need_h == 0) return -1;

    uint64_t bytes64 = (uint64_t)need_w * (uint64_t)need_h * 4ull;
    if (bytes64 == 0 || bytes64 > 0xFFFFFFFFu) return -1;
    const uint32_t need_bytes = (uint32_t)bytes64;

    const int can_reuse = (canvas && shm_fd >= 0 && shm_name[0] != '\0' && need_bytes <= size_bytes);
    if (can_reuse) {
        uint16_t err = 0;
        if (comp_send_attach_shm_name_sync(&conn, surface_id, shm_name, size_bytes, need_w, need_h, need_w, 0u, 2000u, &err) != 0) {
            return -1;
        }
        return 0;
    }

    uint64_t grow64 = (uint64_t)size_bytes * 2ull;
    uint64_t cap64 = (grow64 >= (uint64_t)need_bytes) ? grow64 : (uint64_t)need_bytes;
    if (cap64 > 0xFFFFFFFFu) cap64 = (uint64_t)need_bytes;
    const uint32_t cap_bytes = (uint32_t)cap64;

    char new_name[32];
    new_name[0] = '\0';
    int new_fd = -1;
    for (int i = 0; i < 16; i++) {
        shm_gen++;
        (void)snprintf(new_name, sizeof(new_name), "term_%d_r%d", getpid(), shm_gen);
        new_fd = shm_create_named(new_name, cap_bytes);
        if (new_fd >= 0) break;
    }
    if (new_fd < 0) return -1;

    uint32_t* new_canvas = (uint32_t*)mmap(new_fd, cap_bytes, MAP_SHARED);
    if (!new_canvas) {
        close(new_fd);
        shm_unlink_named(new_name);
        return -1;
    }

    uint16_t err = 0;
    if (comp_send_attach_shm_name_sync(&conn, surface_id, new_name, cap_bytes, need_w, need_h, need_w, 0u, 2000u, &err) != 0) {
        munmap((void*)new_canvas, cap_bytes);
        close(new_fd);
        shm_unlink_named(new_name);
        return -1;
    }

    uint32_t* old_canvas = canvas;
    uint32_t old_size_bytes = size_bytes;
    int old_fd = shm_fd;
    char old_name[32];
    memcpy(old_name, shm_name, sizeof(old_name));

    canvas = new_canvas;
    size_bytes = cap_bytes;
    shm_fd = new_fd;
    memcpy(shm_name, new_name, sizeof(shm_name));

    if (old_canvas) munmap((void*)old_canvas, old_size_bytes);
    if (old_fd >= 0) close(old_fd);
    if (old_name[0]) shm_unlink_named(old_name);

    return 0;
}

static void fb_clear(uint32_t color) {
    if (!canvas) return;
    const uint32_t n = (uint32_t)WIN_W * (uint32_t)WIN_H;
    for (uint32_t i = 0; i < n; i++) {
        canvas[i] = color;
    }
}

static void draw_char_scaled(uint32_t* buf, int w, int h, int x, int y, char c, uint32_t color, int scale) {
    if (!buf || w <= 0 || h <= 0) return;
    if (scale <= 0) return;
    if (c < 0) c = '?';
    const uint8_t* glyph = font8x8_basic[(int)c];

    for (int row = 0; row < 8; row++) {
        const uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            if (((bits >> (7 - col)) & 1u) == 0u) continue;

            const int px0 = x + col * scale;
            const int py0 = y + row * scale;
            for (int sy = 0; sy < scale; sy++) {
                const int py = py0 + sy;
                if ((unsigned)py >= (unsigned)h) continue;
                uint32_t* dst = &buf[py * w];
                for (int sx = 0; sx < scale; sx++) {
                    const int px = px0 + sx;
                    if ((unsigned)px >= (unsigned)w) continue;
                    dst[px] = color;
                }
            }
        }
    }
}

static void fb_fill_rect(int x, int y, int w, int h, uint32_t color) {
    if (!canvas) return;
    if (w <= 0 || h <= 0) return;

    int x0 = x;
    int y0 = y;
    int x1 = x + w;
    int y1 = y + h;

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > WIN_W) x1 = WIN_W;
    if (y1 > WIN_H) y1 = WIN_H;

    for (int yy = y0; yy < y1; yy++) {
        uint32_t* row = &canvas[(uint32_t)yy * (uint32_t)WIN_W];
        for (int xx = x0; xx < x1; xx++) {
            row[xx] = color;
        }
    }
}

static void fb_scroll_view_up(int x0, int y0, int w, int h, int dy, uint32_t fill_color) {
    if (!canvas) return;
    if (w <= 0 || h <= 0) return;

    if (dy <= 0) return;
    if (dy >= h) {
        fb_fill_rect(x0, y0, w, h, fill_color);
        return;
    }

    const size_t row_bytes = (size_t)w * sizeof(uint32_t);
    for (int y = 0; y < h - dy; y++) {
        uint32_t* dst = &canvas[(uint32_t)(y0 + y) * (uint32_t)WIN_W + (uint32_t)x0];
        uint32_t* src = &canvas[(uint32_t)(y0 + y + dy) * (uint32_t)WIN_W + (uint32_t)x0];
        memmove(dst, src, row_bytes);
    }

    fb_fill_rect(x0, y0 + (h - dy), w, dy, fill_color);
}

static void term_calc_view(int* out_x0, int* out_y0, int* out_w, int* out_h) {
    if (out_x0) *out_x0 = TERM_PAD_X;
    if (out_y0) *out_y0 = TERM_PAD_Y;

    int w = WIN_W - (TERM_PAD_X * 2);
    int h = WIN_H - (TERM_PAD_Y * 2);
    if (w < 1) w = 1;
    if (h < 1) h = 1;

    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
}

static void term_calc_grid(int* out_cols, int* out_rows) {
    if (!out_cols || !out_rows) return;

    const int scale = (TERM_SCALE < TERM_SCALE_MIN) ? TERM_SCALE_MIN :
                      (TERM_SCALE > TERM_SCALE_MAX) ? TERM_SCALE_MAX :
                      TERM_SCALE;

    const int cell_w = 8 * scale;
    const int cell_h = 8 * scale;

    int vx = 0, vy = 0, vw = 0, vh = 0;
    term_calc_view(&vx, &vy, &vw, &vh);
    (void)vx;
    (void)vy;

    int cols = vw / cell_w;
    int rows = vh / cell_h;
    if (cols <= 0) cols = 1;
    if (rows <= 0) rows = 1;

    *out_cols = cols;
    *out_rows = rows;
}

static int write_all(int fd, const void* buf, uint32_t size) {
    if (fd < 0) return -1;
    if (!buf || size == 0) return 0;

    const uint8_t* p = (const uint8_t*)buf;
    uint32_t done = 0;
    while (done < size) {
        int r = write(fd, p + done, size - done);
        if (r <= 0) return -1;
        done += (uint32_t)r;
    }
    return (int)done;
}

static inline int ptr_is_invalid(const void* p) {
    return !p || p == (const void*)-1;
}

typedef struct {
    char ch;
    uint32_t fg;
    uint32_t bg;
} term_cell_t;

#define TERM_SCROLLBACK_MAX_LINES 2048u

typedef struct {
    int cols;
    int rows;

    int cur_x;
    int cur_y;

    int saved_x;
    int saved_y;

    uint32_t cur_fg;
    uint32_t cur_bg;

    int cursor_visible;

    term_cell_t* cells;
    uint8_t* dirty_rows;
    int full_redraw;

    term_cell_t** sb_lines;
    uint16_t* sb_cols;
    uint32_t sb_cap;
    uint32_t sb_start;
    uint32_t sb_count;
    uint32_t sb_view_offset;

    int scroll_pending_lines;

    int esc_state;
    int csi_private;
    int csi_params[8];
    int csi_param_count;
    int csi_param_value;
    int csi_in_param;
    int sgr_bright;

    int osc_esc;
} term_t;

static uint32_t term_collect_damage(const term_t* t, comp_ipc_rect_t* rects, uint32_t cap) {
    if (!t || !rects || cap == 0u) return 0u;

    if (t->full_redraw) {
        rects[0].x = 0;
        rects[0].y = 0;
        rects[0].w = WIN_W;
        rects[0].h = WIN_H;
        return 1u;
    }

    int vx = 0, vy = 0, vw = 0, vh = 0;
    term_calc_view(&vx, &vy, &vw, &vh);
    if (vw <= 0 || vh <= 0) return 0u;

    const int scale = (TERM_SCALE < TERM_SCALE_MIN) ? TERM_SCALE_MIN :
                      (TERM_SCALE > TERM_SCALE_MAX) ? TERM_SCALE_MAX :
                      TERM_SCALE;

    const int cell_h = 8 * scale;
    int view_rows = vh / cell_h;
    if (view_rows <= 0) return 0u;

    int rows = (t->rows < view_rows) ? t->rows : view_rows;
    if (rows <= 0 || !t->dirty_rows) return 0u;

    uint32_t n = 0u;
    int run_start = -1;

    for (int y = 0; y < rows; y++) {
        if (t->dirty_rows[y]) {
            if (run_start < 0) run_start = y;
            continue;
        }
        if (run_start < 0) continue;

        int run_len = y - run_start;
        if (run_len > 0) {
            int x0 = vx;
            int y0 = vy + run_start * cell_h;
            int x1 = vx + vw;
            int y1 = y0 + run_len * cell_h;

            if (x0 < 0) x0 = 0;
            if (y0 < 0) y0 = 0;
            if (x1 > WIN_W) x1 = WIN_W;
            if (y1 > WIN_H) y1 = WIN_H;

            if (x1 > x0 && y1 > y0) {
                if (n >= cap) {
                    rects[0].x = 0;
                    rects[0].y = 0;
                    rects[0].w = WIN_W;
                    rects[0].h = WIN_H;
                    return 1u;
                }
                rects[n].x = x0;
                rects[n].y = y0;
                rects[n].w = x1 - x0;
                rects[n].h = y1 - y0;
                n++;
            }
        }
        run_start = -1;
    }

    if (run_start >= 0) {
        int run_len = rows - run_start;
        if (run_len > 0) {
            int x0 = vx;
            int y0 = vy + run_start * cell_h;
            int x1 = vx + vw;
            int y1 = y0 + run_len * cell_h;

            if (x0 < 0) x0 = 0;
            if (y0 < 0) y0 = 0;
            if (x1 > WIN_W) x1 = WIN_W;
            if (y1 > WIN_H) y1 = WIN_H;

            if (x1 > x0 && y1 > y0) {
                if (n >= cap) {
                    rects[0].x = 0;
                    rects[0].y = 0;
                    rects[0].w = WIN_W;
                    rects[0].h = WIN_H;
                    return 1u;
                }
                rects[n].x = x0;
                rects[n].y = y0;
                rects[n].w = x1 - x0;
                rects[n].h = y1 - y0;
                n++;
            }
        }
    }

    return n;
}

static const uint32_t TERM_DEF_FG = 0xD4D4D4u;
static const uint32_t TERM_DEF_BG = 0x111111u;

static const uint32_t ansi_colors[8] = {
    0x000000u,
    0xCC0000u,
    0x00CC00u,
    0xCCCC00u,
    0x0000CCu,
    0xCC00CCu,
    0x00CCCCu,
    0xCCCCCCu,
};

static const uint32_t ansi_bright_colors[8] = {
    0x666666u,
    0xFF3333u,
    0x33FF33u,
    0xFFFF33u,
    0x3333FFu,
    0xFF33FFu,
    0x33FFFFu,
    0xFFFFFFu,
};

static inline uint32_t term_cells_count(int cols, int rows) {
    if (cols <= 0 || rows <= 0) return 0;
    uint64_t n = (uint64_t)(uint32_t)cols * (uint64_t)(uint32_t)rows;
    if (n == 0 || n > 0xFFFFFFFFu) return 0;
    return (uint32_t)n;
}

static inline void term_mark_dirty(term_t* t, int y) {
    if (!t || !t->dirty_rows) return;
    if ((unsigned)y >= (unsigned)t->rows) return;
    t->dirty_rows[y] = 1u;
}

static void term_mark_all_dirty(term_t* t) {
    if (!t || !t->dirty_rows) return;
    for (int y = 0; y < t->rows; y++) {
        t->dirty_rows[y] = 1u;
    }
}

static uint32_t term_scrollback_total_lines(const term_t* t) {
    if (!t) return 0;
    return t->sb_count + (uint32_t)t->rows;
}

static uint32_t term_scrollback_max_offset(const term_t* t, uint32_t view_rows) {
    if (!t) return 0;
    const uint32_t total = term_scrollback_total_lines(t);
    if (total <= view_rows) return 0;
    return total - view_rows;
}

static void term_scrollback_clamp_view(term_t* t, uint32_t view_rows) {
    if (!t) return;
    uint32_t max_off = term_scrollback_max_offset(t, view_rows);
    if (t->sb_view_offset > max_off) t->sb_view_offset = max_off;
}

static void term_scrollback_on_append(term_t* t, uint32_t appended_lines, uint32_t view_rows) {
    if (!t) return;
    if (appended_lines == 0) return;

    if (t->sb_view_offset > 0) {
        uint32_t new_off = t->sb_view_offset + appended_lines;
        t->sb_view_offset = new_off;
    }
    term_scrollback_clamp_view(t, view_rows);
}

static void term_scrollback_push_line(term_t* t, const term_cell_t* line, uint16_t cols) {
    if (!t || !line) return;
    if (!t->sb_lines || !t->sb_cols || t->sb_cap == 0) return;
    if (cols == 0) return;

    term_cell_t* copy = (term_cell_t*)malloc((size_t)cols * sizeof(*copy));
    if (ptr_is_invalid(copy)) return;
    memcpy(copy, line, (size_t)cols * sizeof(*copy));

    uint32_t idx = 0;
    if (t->sb_count < t->sb_cap) {
        idx = (t->sb_start + t->sb_count) % t->sb_cap;
        t->sb_count++;
    } else {
        idx = t->sb_start;
        if (t->sb_lines[idx] && !ptr_is_invalid(t->sb_lines[idx])) {
            free(t->sb_lines[idx]);
        }
        t->sb_start = (t->sb_start + 1u) % t->sb_cap;
    }

    t->sb_lines[idx] = copy;
    t->sb_cols[idx] = cols;
}

static void term_scrollback_scroll(term_t* t, int delta_lines, uint32_t view_rows) {
    if (!t || delta_lines == 0) return;

    if (delta_lines > 0) {
        uint32_t d = (uint32_t)delta_lines;
        uint32_t max_off = term_scrollback_max_offset(t, view_rows);
        uint32_t new_off = t->sb_view_offset + d;
        t->sb_view_offset = (new_off > max_off) ? max_off : new_off;
        return;
    }

    uint32_t d = (uint32_t)(-delta_lines);
    t->sb_view_offset = (t->sb_view_offset > d) ? (t->sb_view_offset - d) : 0u;
}

static void term_scrollback_reset(term_t* t) {
    if (!t) return;
    if (t->sb_lines && !ptr_is_invalid(t->sb_lines)) {
        for (uint32_t i = 0; i < t->sb_cap; i++) {
            if (t->sb_lines[i] && !ptr_is_invalid(t->sb_lines[i])) {
                free(t->sb_lines[i]);
            }
            t->sb_lines[i] = 0;
        }
    }
    if (t->sb_cols && !ptr_is_invalid(t->sb_cols)) {
        memset(t->sb_cols, 0, (size_t)t->sb_cap * sizeof(*t->sb_cols));
    }
    t->sb_start = 0;
    t->sb_count = 0;
    t->sb_view_offset = 0;
}

static void term_free(term_t* t) {
    if (!t) return;
    if (t->cells && !ptr_is_invalid(t->cells)) {
        free(t->cells);
    }
    if (t->dirty_rows && !ptr_is_invalid(t->dirty_rows)) {
        free(t->dirty_rows);
    }
    if (t->sb_lines && !ptr_is_invalid(t->sb_lines)) {
        for (uint32_t i = 0; i < t->sb_cap; i++) {
            if (t->sb_lines[i] && !ptr_is_invalid(t->sb_lines[i])) {
                free(t->sb_lines[i]);
            }
        }
        free(t->sb_lines);
    }
    if (t->sb_cols && !ptr_is_invalid(t->sb_cols)) {
        free(t->sb_cols);
    }
    memset(t, 0, sizeof(*t));
}

static void term_clear(term_t* t) {
    if (!t || !t->cells) return;
    const uint32_t n = term_cells_count(t->cols, t->rows);
    if (n == 0) return;

    for (uint32_t i = 0; i < n; i++) {
        t->cells[i].ch = ' ';
        t->cells[i].fg = t->cur_fg;
        t->cells[i].bg = t->cur_bg;
    }
    t->cur_x = 0;
    t->cur_y = 0;
    t->scroll_pending_lines = 0;
    term_mark_all_dirty(t);
}

static void term_scroll_up(term_t* t, int lines) {
    if (!t || !t->cells) return;
    if (lines <= 0) return;
    if (lines > t->rows) lines = t->rows;

    const int cols = t->cols;
    const int rows = t->rows;
    const uint32_t row_bytes = (uint32_t)cols * (uint32_t)sizeof(term_cell_t);

    for (int i = 0; i < lines; i++) {
        term_scrollback_push_line(t, &t->cells[i * cols], (uint16_t)cols);
    }

    if (lines < rows) {
        memmove(&t->cells[0], &t->cells[lines * cols], (uint32_t)(rows - lines) * row_bytes);
        if (t->dirty_rows) {
            memmove(&t->dirty_rows[0], &t->dirty_rows[lines], (uint32_t)(rows - lines));
            memset(&t->dirty_rows[rows - lines], 1, (size_t)lines);
        }
    } else {
        if (t->dirty_rows) {
            memset(t->dirty_rows, 1, (size_t)rows);
        }
    }

    for (int y = rows - lines; y < rows; y++) {
        term_cell_t* row = &t->cells[y * cols];
        for (int x = 0; x < cols; x++) {
            row[x].ch = ' ';
            row[x].fg = t->cur_fg;
            row[x].bg = t->cur_bg;
        }
    }

    t->scroll_pending_lines += lines;
    if (t->scroll_pending_lines > rows) t->scroll_pending_lines = rows;

    term_scrollback_on_append(t, (uint32_t)lines, (uint32_t)rows);
}

static int term_resize(term_t* t, int cols, int rows) {
    if (!t) return -1;
    if (cols <= 0 || rows <= 0) return -1;

    const uint32_t n = term_cells_count(cols, rows);
    if (n == 0) return -1;

    term_cell_t* new_cells = (term_cell_t*)calloc(n, sizeof(term_cell_t));
    if (ptr_is_invalid(new_cells)) return -1;

    uint8_t* new_dirty = (uint8_t*)calloc((size_t)rows, 1u);
    if (ptr_is_invalid(new_dirty)) {
        free(new_cells);
        return -1;
    }

    const int old_cols = t->cols;
    const int old_rows = t->rows;
    term_cell_t* old_cells = t->cells;

    for (uint32_t i = 0; i < n; i++) {
        new_cells[i].ch = ' ';
        new_cells[i].fg = t->cur_fg;
        new_cells[i].bg = t->cur_bg;
    }

    int copy_cols = (old_cols < cols) ? old_cols : cols;
    int copy_rows = (old_rows < rows) ? old_rows : rows;
    int old_y_start = 0;

    if (old_cells && old_cols > 0 && old_rows > 0) {
        if (rows < old_rows) {
            const int removed = old_rows - rows;
            for (int y = 0; y < removed; y++) {
                term_scrollback_push_line(t, &old_cells[y * old_cols], (uint16_t)old_cols);
            }
            term_scrollback_on_append(t, (uint32_t)removed, (uint32_t)rows);
            old_y_start = removed;
            copy_rows = rows;
        }

        for (int y = 0; y < copy_rows; y++) {
            const int oy = old_y_start + y;
            for (int x = 0; x < copy_cols; x++) {
                new_cells[y * cols + x] = old_cells[oy * old_cols + x];
            }
        }
    }

    if (t->cells && !ptr_is_invalid(t->cells)) free(t->cells);
    if (t->dirty_rows && !ptr_is_invalid(t->dirty_rows)) free(t->dirty_rows);

    t->cells = new_cells;
    t->dirty_rows = new_dirty;
    t->cols = cols;
    t->rows = rows;

    if (old_rows > 0 && rows < old_rows) {
        t->cur_y -= (old_rows - rows);
        if (t->cur_y < 0) t->cur_y = 0;
    }
    if (t->cur_x >= cols) t->cur_x = cols - 1;
    if (t->cur_y >= rows) t->cur_y = rows - 1;
    t->scroll_pending_lines = 0;
    t->full_redraw = 1;
    term_mark_all_dirty(t);
    return 0;
}

static void term_init(term_t* t, int cols, int rows) {
    if (!t) return;
    memset(t, 0, sizeof(*t));
    t->cur_fg = TERM_DEF_FG;
    t->cur_bg = TERM_DEF_BG;
    t->cursor_visible = 1;
    t->sb_cap = TERM_SCROLLBACK_MAX_LINES;
    t->sb_lines = (term_cell_t**)calloc((size_t)t->sb_cap, sizeof(*t->sb_lines));
    if (ptr_is_invalid(t->sb_lines)) {
        t->sb_lines = 0;
        t->sb_cap = 0;
    }
    if (t->sb_cap != 0) {
        t->sb_cols = (uint16_t*)calloc((size_t)t->sb_cap, sizeof(*t->sb_cols));
        if (ptr_is_invalid(t->sb_cols)) {
            if (t->sb_lines) free(t->sb_lines);
            t->sb_lines = 0;
            t->sb_cols = 0;
            t->sb_cap = 0;
        }
    }
    t->sb_start = 0;
    t->sb_count = 0;
    t->sb_view_offset = 0;
    (void)term_resize(t, cols, rows);
    term_clear(t);
}

static inline int term_idx(const term_t* t, int x, int y) {
    return y * t->cols + x;
}

static void term_set_cursor(term_t* t, int x, int y) {
    if (!t) return;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= t->cols) x = t->cols - 1;
    if (y >= t->rows) y = t->rows - 1;

    if (y != t->cur_y) {
        term_mark_dirty(t, t->cur_y);
        term_mark_dirty(t, y);
    } else {
        term_mark_dirty(t, y);
    }

    t->cur_x = x;
    t->cur_y = y;
}

static void term_newline(term_t* t) {
    if (!t) return;
    term_mark_dirty(t, t->cur_y);
    t->cur_x = 0;
    t->cur_y++;
    if (t->cur_y >= t->rows) {
        term_scroll_up(t, 1);
        t->cur_y = t->rows - 1;
    }
    term_mark_dirty(t, t->cur_y);
}

static void term_put_cell(term_t* t, int x, int y, char ch, uint32_t fg, uint32_t bg) {
    if (!t || !t->cells) return;
    if ((unsigned)x >= (unsigned)t->cols || (unsigned)y >= (unsigned)t->rows) return;
    term_cell_t* c = &t->cells[term_idx(t, x, y)];
    c->ch = ch;
    c->fg = fg;
    c->bg = bg;
    term_mark_dirty(t, y);
}

static void term_put_char(term_t* t, char ch) {
    if (!t || !t->cells) return;

    if ((uint8_t)ch == 0x0Cu) {
        term_scrollback_reset(t);
        term_clear(t);
        return;
    }

    if (ch == '\r') {
        term_set_cursor(t, 0, t->cur_y);
        return;
    }
    if (ch == '\n') {
        term_newline(t);
        return;
    }
    if (ch == '\b') {
        if (t->cur_x > 0) {
            term_set_cursor(t, t->cur_x - 1, t->cur_y);
        }
        return;
    }
    if (ch == '\t') {
        int next = (t->cur_x + 8) & ~7;
        if (next <= t->cur_x) next = t->cur_x + 1;
        while (t->cur_x < next) {
            if (t->cur_x >= t->cols) {
                term_newline(t);
                break;
            }
            term_put_cell(t, t->cur_x, t->cur_y, ' ', t->cur_fg, t->cur_bg);
            t->cur_x++;
        }
        return;
    }

    if ((uint8_t)ch < 32u) {
        return;
    }

    if (t->cur_x >= t->cols) {
        term_newline(t);
    }

    term_put_cell(t, t->cur_x, t->cur_y, ch, t->cur_fg, t->cur_bg);
    t->cur_x++;
    if (t->cur_x >= t->cols) {
        term_newline(t);
    }
}

static void term_erase_in_line(term_t* t, int mode) {
    if (!t || !t->cells) return;

    int x0 = 0;
    int x1 = t->cols;

    if (mode == 0) {
        x0 = t->cur_x;
    } else if (mode == 1) {
        x1 = t->cur_x + 1;
    } else {
        x0 = 0;
        x1 = t->cols;
    }

    if (x0 < 0) x0 = 0;
    if (x1 > t->cols) x1 = t->cols;
    if (x0 >= x1) return;

    term_cell_t* row = &t->cells[t->cur_y * t->cols];
    for (int x = x0; x < x1; x++) {
        row[x].ch = ' ';
        row[x].fg = t->cur_fg;
        row[x].bg = t->cur_bg;
    }
    term_mark_dirty(t, t->cur_y);
}

static void term_erase_in_display(term_t* t, int mode) {
    if (!t || !t->cells) return;

    if (mode == 2) {
        term_clear(t);
        return;
    }

    if (mode == 0) {
        term_erase_in_line(t, 0);
        for (int y = t->cur_y + 1; y < t->rows; y++) {
            term_cell_t* row = &t->cells[y * t->cols];
            for (int x = 0; x < t->cols; x++) {
                row[x].ch = ' ';
                row[x].fg = t->cur_fg;
                row[x].bg = t->cur_bg;
            }
            term_mark_dirty(t, y);
        }
        return;
    }

    if (mode == 1) {
        term_erase_in_line(t, 1);
        for (int y = 0; y < t->cur_y; y++) {
            term_cell_t* row = &t->cells[y * t->cols];
            for (int x = 0; x < t->cols; x++) {
                row[x].ch = ' ';
                row[x].fg = t->cur_fg;
                row[x].bg = t->cur_bg;
            }
            term_mark_dirty(t, y);
        }
        return;
    }
}

static void term_escape_reset(term_t* t) {
    if (!t) return;
    t->esc_state = 0;
    t->csi_private = 0;
    t->csi_param_count = 0;
    t->csi_param_value = 0;
    t->csi_in_param = 0;
    t->osc_esc = 0;
}

static inline int term_csi_param(const term_t* t, int idx, int def) {
    if (!t) return def;
    if (idx < 0 || idx >= t->csi_param_count) return def;
    int v = t->csi_params[idx];
    return (v == 0) ? def : v;
}

static void term_csi_finish(term_t* t, char cmd) {
    if (!t) return;

    switch (cmd) {
        case 'A': {
            int n = term_csi_param(t, 0, 1);
            term_set_cursor(t, t->cur_x, t->cur_y - n);
        } break;
        case 'B': {
            int n = term_csi_param(t, 0, 1);
            term_set_cursor(t, t->cur_x, t->cur_y + n);
        } break;
        case 'C': {
            int n = term_csi_param(t, 0, 1);
            term_set_cursor(t, t->cur_x + n, t->cur_y);
        } break;
        case 'D': {
            int n = term_csi_param(t, 0, 1);
            term_set_cursor(t, t->cur_x - n, t->cur_y);
        } break;
        case 'H':
        case 'f': {
            int row = term_csi_param(t, 0, 1);
            int col = term_csi_param(t, 1, 1);
            term_set_cursor(t, col - 1, row - 1);
        } break;
        case 'J': {
            int mode = (t->csi_param_count > 0) ? t->csi_params[0] : 0;
            term_erase_in_display(t, mode);
        } break;
        case 'K': {
            int mode = (t->csi_param_count > 0) ? t->csi_params[0] : 0;
            term_erase_in_line(t, mode);
        } break;
        case 's':
            t->saved_x = t->cur_x;
            t->saved_y = t->cur_y;
            break;
        case 'u':
            term_set_cursor(t, t->saved_x, t->saved_y);
            break;
        case 'm': {
            int count = t->csi_param_count;
            if (count == 0) {
                t->cur_fg = TERM_DEF_FG;
                t->cur_bg = TERM_DEF_BG;
                t->sgr_bright = 0;
                break;
            }
            for (int i = 0; i < count; i++) {
                int p = t->csi_params[i];
                if (p == 0) {
                    t->cur_fg = TERM_DEF_FG;
                    t->cur_bg = TERM_DEF_BG;
                    t->sgr_bright = 0;
                } else if (p == 1) {
                    t->sgr_bright = 1;
                } else if (p == 22) {
                    t->sgr_bright = 0;
                } else if (p == 39) {
                    t->cur_fg = TERM_DEF_FG;
                } else if (p == 49) {
                    t->cur_bg = TERM_DEF_BG;
                } else if (p >= 30 && p <= 37) {
                    int idx = p - 30;
                    t->cur_fg = t->sgr_bright ? ansi_bright_colors[idx] : ansi_colors[idx];
                } else if (p >= 40 && p <= 47) {
                    int idx = p - 40;
                    t->cur_bg = ansi_colors[idx];
                } else if (p >= 90 && p <= 97) {
                    int idx = p - 90;
                    t->cur_fg = ansi_bright_colors[idx];
                } else if (p >= 100 && p <= 107) {
                    int idx = p - 100;
                    t->cur_bg = ansi_bright_colors[idx];
                }
            }
        } break;
        default:
            break;
    }

    term_escape_reset(t);
}

static void term_process_byte(term_t* t, char ch) {
    if (!t) return;

    if (t->esc_state == 0) {
        if ((uint8_t)ch == 0x1Bu) {
            t->esc_state = 1;
            return;
        }
        term_put_char(t, ch);
        return;
    }

    if (t->esc_state == 1) {
        if (ch == '[') {
            t->esc_state = 2;
            t->csi_private = 0;
            t->csi_param_count = 0;
            t->csi_param_value = 0;
            t->csi_in_param = 0;
            return;
        }
        if (ch == ']') {
            t->esc_state = 3;
            t->osc_esc = 0;
            return;
        }
        if (ch == '7') {
            t->saved_x = t->cur_x;
            t->saved_y = t->cur_y;
            term_escape_reset(t);
            return;
        }
        if (ch == '8') {
            term_set_cursor(t, t->saved_x, t->saved_y);
            term_escape_reset(t);
            return;
        }
        term_escape_reset(t);
        return;
    }

    if (t->esc_state == 3) {
        if ((uint8_t)ch == 0x07u) {
            term_escape_reset(t);
            return;
        }
        if (t->osc_esc) {
            if (ch == '\\') {
                term_escape_reset(t);
                return;
            }
            t->osc_esc = 0;
        }
        if ((uint8_t)ch == 0x1Bu) {
            t->osc_esc = 1;
        }
        return;
    }

    if (t->esc_state != 2) {
        term_escape_reset(t);
        return;
    }

    if (ch == '?' && t->csi_param_count == 0 && !t->csi_in_param) {
        t->csi_private = 1;
        return;
    }

    if (ch >= '0' && ch <= '9') {
        t->csi_in_param = 1;
        t->csi_param_value = t->csi_param_value * 10 + (int)(ch - '0');
        if (t->csi_param_value > 9999) t->csi_param_value = 9999;
        return;
    }

    if (ch == ';') {
        if (t->csi_param_count < (int)(sizeof(t->csi_params) / sizeof(t->csi_params[0]))) {
            t->csi_params[t->csi_param_count++] = t->csi_in_param ? t->csi_param_value : 0;
        }
        t->csi_param_value = 0;
        t->csi_in_param = 0;
        return;
    }

    if (t->csi_in_param || t->csi_param_count > 0) {
        if (t->csi_param_count < (int)(sizeof(t->csi_params) / sizeof(t->csi_params[0]))) {
            t->csi_params[t->csi_param_count++] = t->csi_in_param ? t->csi_param_value : 0;
        }
    }

    if (t->csi_private && (ch == 'h' || ch == 'l')) {
        int p0 = (t->csi_param_count > 0) ? t->csi_params[0] : 0;
        if (p0 == 25) {
            t->cursor_visible = (ch == 'h');
            term_mark_dirty(t, t->cur_y);
        }
        term_escape_reset(t);
        return;
    }

    term_csi_finish(t, ch);
}

static void term_process_buf(term_t* t, const void* buf, uint32_t size) {
    if (!t || !buf || size == 0) return;
    const char* p = (const char*)buf;
    for (uint32_t i = 0; i < size; i++) {
        term_process_byte(t, p[i]);
    }
}

static void term_render(term_t* t) {
    if (!t || !canvas || !t->cells || !t->dirty_rows) return;

    const int scale = (TERM_SCALE < TERM_SCALE_MIN) ? TERM_SCALE_MIN :
                      (TERM_SCALE > TERM_SCALE_MAX) ? TERM_SCALE_MAX :
                      TERM_SCALE;

    const int cell_w = 8 * scale;
    const int cell_h = 8 * scale;

    int vx = 0, vy = 0, vw = 0, vh = 0;
    term_calc_view(&vx, &vy, &vw, &vh);

    const int view_cols = vw / cell_w;
    const int view_rows = vh / cell_h;

    if (view_cols <= 0 || view_rows <= 0) return;

    const int cols = (t->cols < view_cols) ? t->cols : view_cols;
    const int rows = (t->rows < view_rows) ? t->rows : view_rows;

    if (t->full_redraw) {
        fb_clear(t->cur_bg);
        term_mark_all_dirty(t);
        t->scroll_pending_lines = 0;
        t->full_redraw = 0;
    }

    if (t->sb_view_offset == 0 && t->scroll_pending_lines > 0 && rows > 0) {
        int lines = t->scroll_pending_lines;
        if (lines > rows) lines = rows;

        const int dy = lines * cell_h;
        if (dy >= vh) {
            fb_fill_rect(vx, vy, vw, vh, t->cur_bg);
            term_mark_all_dirty(t);
        } else if (dy > 0) {
            fb_scroll_view_up(vx, vy, vw, vh, dy, t->cur_bg);
        }

        t->scroll_pending_lines = 0;
    }

    if (t->sb_view_offset > 0) {
        t->scroll_pending_lines = 0;
    }

    term_scrollback_clamp_view(t, (uint32_t)rows);
    const uint32_t view_rows_u = (uint32_t)rows;
    const uint32_t total_lines = term_scrollback_total_lines(t);
    const uint32_t start_line = (total_lines > (view_rows_u + t->sb_view_offset))
                                ? (total_lines - view_rows_u - t->sb_view_offset)
                                : 0u;

    for (int y = 0; y < rows; y++) {
        if (!t->dirty_rows[y]) continue;

        const int py = vy + y * cell_h;
        fb_fill_rect(vx, py, vw, cell_h, t->cur_bg);

        const uint32_t line_no = start_line + (uint32_t)y;
        const term_cell_t* src_line = 0;
        uint16_t src_cols = 0;
        if (line_no < t->sb_count && t->sb_cap != 0 && t->sb_lines && t->sb_cols) {
            uint32_t idx = (t->sb_start + line_no) % t->sb_cap;
            src_line = t->sb_lines[idx];
            src_cols = t->sb_cols[idx];
        } else {
            uint32_t screen_y = (line_no > t->sb_count) ? (line_no - t->sb_count) : 0u;
            if (screen_y < (uint32_t)t->rows) {
                src_line = &t->cells[screen_y * (uint32_t)t->cols];
                src_cols = (uint16_t)t->cols;
            }
        }

        for (int x = 0; x < cols; x++) {
            term_cell_t c;
            if (src_line && (uint16_t)x < src_cols) {
                c = src_line[x];
            } else {
                c.ch = ' ';
                c.fg = t->cur_fg;
                c.bg = t->cur_bg;
            }
            if (c.bg != t->cur_bg) {
                fb_fill_rect(vx + x * cell_w, py, cell_w, cell_h, c.bg);
            }
            const char ch = c.ch ? c.ch : ' ';
            if (ch != ' ') {
                draw_char_scaled(canvas, WIN_W, WIN_H, vx + x * cell_w, py, ch, c.fg, scale);
            }
        }

        t->dirty_rows[y] = 0u;
    }

    if (t->cursor_visible && t->sb_view_offset == 0) {
        const int cx = t->cur_x;
        const int cy = t->cur_y;
        if ((unsigned)cx < (unsigned)cols && (unsigned)cy < (unsigned)rows) {
            const term_cell_t c = t->cells[term_idx(t, cx, cy)];
            const char ch = c.ch ? c.ch : ' ';
            const int px = vx + cx * cell_w;
            const int py = vy + cy * cell_h;
            fb_fill_rect(px, py, cell_w, cell_h, c.fg);
            if (ch != ' ') {
                draw_char_scaled(canvas, WIN_W, WIN_H, px, py, ch, c.bg, scale);
            }
        }
    }
}

static int term_run(void) {
    term_t term;
    memset(&term, 0, sizeof(term));

    int master_fd = -1;
    int child_pid = -1;
    int rc = 0;

    comp_conn_reset(&conn);
    if (comp_connect(&conn, "flux") != 0) {
        return 1;
    }

    {
        uint16_t err = 0;
        if (comp_send_hello_sync(&conn, 2000u, &err) != 0) {
            comp_disconnect(&conn);
            return 1;
        }
    }

    if (ensure_surface((uint32_t)WIN_W, (uint32_t)WIN_H) != 0) {
        comp_disconnect(&conn);
        return 1;
    }

    {
        int cols = 0;
        int rows = 0;
        term_calc_grid(&cols, &rows);
        term_init(&term, cols, rows);
        term.full_redraw = 1;
        {
            comp_ipc_rect_t rects[COMP_IPC_DAMAGE_MAX_RECTS];
            uint32_t rect_n = term_collect_damage(&term, rects, COMP_IPC_DAMAGE_MAX_RECTS);
            term_render(&term);
            if (rect_n > 0u) {
                (void)comp_send_damage(&conn, surface_id, rects, rect_n);
            }
        }
        (void)comp_send_commit(&conn, surface_id, 0, 0, 0u);
    }

    master_fd = open("/dev/ptmx", 0);
    if (master_fd < 0) {
        rc = 1;
        goto cleanup;
    }

    uint32_t pty_id = 0;
    if (ioctl(master_fd, YOS_TIOCGPTN, &pty_id) != 0 || pty_id == 0) {
        rc = 1;
        goto cleanup;
    }

    char pts_path[64];
    (void)snprintf(pts_path, sizeof(pts_path), "/dev/pts/%u", (unsigned)pty_id);
    int slave_fd = open(pts_path, 0);
    if (slave_fd < 0) {
        rc = 1;
        goto cleanup;
    }

    {
        yos_winsize_t ws;
        ws.ws_col = (uint16_t)term.cols;
        ws.ws_row = (uint16_t)term.rows;
        ws.ws_xpixel = 0;
        ws.ws_ypixel = 0;
        (void)ioctl(slave_fd, YOS_TIOCSWINSZ, &ws);
    }

    if (dup2(slave_fd, 0) < 0 || dup2(slave_fd, 1) < 0 || dup2(slave_fd, 2) < 0) {
        close(slave_fd);
        rc = 1;
        goto cleanup;
    }
    if (slave_fd > 2) close(slave_fd);

    {
        char* sh_argv[1];
        sh_argv[0] = (char*)"ush";
        child_pid = spawn_process_resolved("ush", 1, sh_argv);
    }

    int running = 1;
    int need_update = 0;

    if (child_pid < 0) {
        const char msg[] = "term: failed to spawn ush\n";
        term_process_buf(&term, msg, (uint32_t)(sizeof(msg) - 1u));
        need_update = 1;
    }
    while (running) {
        for (;;) {
            comp_ipc_hdr_t hdr;
            uint8_t payload[COMP_IPC_MAX_PAYLOAD];
            int ir = comp_try_recv(&conn, &hdr, payload, (uint32_t)sizeof(payload));
            if (ir < 0) {
                running = 0;
                break;
            }
            if (ir == 0) break;

            if (hdr.type != (uint16_t)COMP_IPC_MSG_INPUT || hdr.len != (uint32_t)sizeof(comp_ipc_input_t)) {
                continue;
            }

            comp_ipc_input_t in;
            memcpy(&in, payload, sizeof(in));
            if (in.surface_id != surface_id) continue;

            if (in.kind == COMP_IPC_INPUT_KEY && in.key_state == 1u) {
                uint8_t kc = (uint8_t)in.keycode;

                if (kc == 0x8Au || kc == 0x8Bu || kc == 0x8Cu) {
                    int new_scale = TERM_SCALE;
                    if (kc == 0x8Au) new_scale--;
                    else if (kc == 0x8Bu) new_scale++;
                    else new_scale = TERM_SCALE_DEFAULT;

                    if (new_scale < TERM_SCALE_MIN) new_scale = TERM_SCALE_MIN;
                    if (new_scale > TERM_SCALE_MAX) new_scale = TERM_SCALE_MAX;

                    if (new_scale != TERM_SCALE) {
                        TERM_SCALE = new_scale;

                        int cols = 0;
                        int rows = 0;
                        term_calc_grid(&cols, &rows);
                        (void)term_resize(&term, cols, rows);
                        term_scrollback_clamp_view(&term, (uint32_t)term.rows);

                        yos_winsize_t ws;
                        ws.ws_col = (uint16_t)term.cols;
                        ws.ws_row = (uint16_t)term.rows;
                        ws.ws_xpixel = 0;
                        ws.ws_ypixel = 0;
                        (void)ioctl(master_fd, YOS_TIOCSWINSZ, &ws);
                        (void)ioctl(0, YOS_TIOCSWINSZ, &ws);

                        need_update = 1;
                    }
                    continue;
                }

                if (kc == 0x80u || kc == 0x81u) {
                    uint32_t old_off = term.sb_view_offset;
                    term_scrollback_scroll(&term, (kc == 0x80u) ? +1 : -1, (uint32_t)term.rows);
                    if (term.sb_view_offset != old_off) {
                        term.full_redraw = 1;
                        need_update = 1;
                    }
                    continue;
                }

                if (term.sb_view_offset != 0) {
                    term.sb_view_offset = 0;
                    term.full_redraw = 1;
                    need_update = 1;
                }

                if (kc == 0x11u) {
                    const char seq[] = "\x1b[D";
                    (void)write_all(master_fd, seq, (uint32_t)(sizeof(seq) - 1u));
                } else if (kc == 0x12u) {
                    const char seq[] = "\x1b[C";
                    (void)write_all(master_fd, seq, (uint32_t)(sizeof(seq) - 1u));
                } else if (kc == 0x13u) {
                    const char seq[] = "\x1b[A";
                    (void)write_all(master_fd, seq, (uint32_t)(sizeof(seq) - 1u));
                } else if (kc == 0x14u) {
                    const char seq[] = "\x1b[B";
                    (void)write_all(master_fd, seq, (uint32_t)(sizeof(seq) - 1u));
                } else {
                    (void)write_all(master_fd, &kc, 1u);
                }
            } else if (in.kind == COMP_IPC_INPUT_RESIZE) {
                const int new_w = in.x;
                const int new_h = in.y;
                if (new_w > 0 && new_h > 0) {
                    WIN_W = new_w;
                    WIN_H = new_h;

                    if (ensure_surface((uint32_t)WIN_W, (uint32_t)WIN_H) == 0) {
                        int cols = 0;
                        int rows = 0;
                        term_calc_grid(&cols, &rows);
                        (void)term_resize(&term, cols, rows);
                        term.full_redraw = 1;

                        yos_winsize_t ws;
                        ws.ws_col = (uint16_t)term.cols;
                        ws.ws_row = (uint16_t)term.rows;
                        ws.ws_xpixel = 0;
                        ws.ws_ypixel = 0;
                        (void)ioctl(master_fd, YOS_TIOCSWINSZ, &ws);
                        (void)ioctl(0, YOS_TIOCSWINSZ, &ws);

                        need_update = 1;
                    }
                }
            } else if (in.kind == COMP_IPC_INPUT_CLOSE) {
                running = 0;
                break;
            }
        }

        const int comp_fd = conn.fd_s2c_r;
        pollfd_t pfds[2];
        uint32_t nfds = 0;

        pfds[nfds].fd = master_fd;
        pfds[nfds].events = POLLIN;
        pfds[nfds].revents = 0;
        nfds++;

        if (comp_fd >= 0) {
            pfds[nfds].fd = comp_fd;
            pfds[nfds].events = POLLIN;
            pfds[nfds].revents = 0;
            nfds++;
        }

        const int poll_timeout_ms = need_update ? 0 : (conn.input_ring_enabled ? 4 : 50);
        int pr = poll(pfds, nfds, poll_timeout_ms);
        if (pr < 0) {
            running = 0;
        } else if (pr > 0) {
            if (pfds[0].revents & (POLLERR | POLLNVAL)) {
                running = 0;
            } else if (pfds[0].revents & POLLIN) {
                for (;;) {
                    char buf[1024];
                    int rn = read(master_fd, buf, (uint32_t)sizeof(buf));
                    if (rn > 0) {
                        term_process_buf(&term, buf, (uint32_t)rn);
                        need_update = 1;
                        if (rn < (int)sizeof(buf)) break;
                        continue;
                    }
                    running = 0;
                    break;
                }
            } else if (pfds[0].revents & POLLHUP) {
                running = 0;
            }

            if (nfds > 1 && (pfds[1].revents & (POLLERR | POLLNVAL | POLLHUP))) {
                running = 0;
            }
        }

        if (need_update) {
            comp_ipc_rect_t rects[COMP_IPC_DAMAGE_MAX_RECTS];
            uint32_t rect_n = term_collect_damage(&term, rects, COMP_IPC_DAMAGE_MAX_RECTS);
            term_render(&term);
            if (rect_n > 0u) {
                (void)comp_send_damage(&conn, surface_id, rects, rect_n);
            }
            (void)comp_send_commit(&conn, surface_id, 0, 0, 0u);
            need_update = 0;
        }

    }

cleanup:
    if (master_fd >= 0) {
        close(master_fd);
        master_fd = -1;
    }

    if (conn.connected) {
        uint16_t err = 0;
        (void)comp_send_destroy_surface_sync(&conn, surface_id, 0u, 2000u, &err);
    }

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
    size_bytes = 0;

    term_free(&term);
    comp_disconnect(&conn);
    return rc;
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    return term_run();
}
