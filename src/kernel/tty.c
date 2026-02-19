#include <kernel/tty.h>
#include <kernel/proc.h>

#include <hal/lock.h>
#include <lib/string.h>
#include <drivers/fbdev.h>

#include <mm/heap.h>

static spinlock_t tty_lock;
static term_instance_t* tty_term;

static term_instance_t tty_snapshot_term;
static char* tty_snapshot_buf;
static uint32_t* tty_snapshot_fg;
static uint32_t* tty_snapshot_bg;
static size_t tty_snapshot_cap_cells;

static uint8_t* tty_snapshot_dirty_rows;
static int* tty_snapshot_dirty_x1;
static int* tty_snapshot_dirty_x2;
static int tty_snapshot_cap_rows;

static int tty_snapshot_reserve(size_t cells) {
    if (cells <= tty_snapshot_cap_cells) return 0;

    size_t new_cap = tty_snapshot_cap_cells ? tty_snapshot_cap_cells : 1024u;
    while (new_cap < cells) {
        size_t next = new_cap << 1;
        if (next <= new_cap) {
            new_cap = cells;
            break;
        }
        new_cap = next;
    }

    char* nb = (char*)kmalloc(new_cap ? new_cap : 1);
    uint32_t* nfg = (uint32_t*)kmalloc((new_cap ? new_cap : 1) * sizeof(uint32_t));
    uint32_t* nbg = (uint32_t*)kmalloc((new_cap ? new_cap : 1) * sizeof(uint32_t));
    if (!nb || !nfg || !nbg) {
        if (nb) kfree(nb);
        if (nfg) kfree(nfg);
        if (nbg) kfree(nbg);
        return -1;
    }

    if (tty_snapshot_buf) kfree(tty_snapshot_buf);
    if (tty_snapshot_fg) kfree(tty_snapshot_fg);
    if (tty_snapshot_bg) kfree(tty_snapshot_bg);

    tty_snapshot_buf = nb;
    tty_snapshot_fg = nfg;
    tty_snapshot_bg = nbg;
    tty_snapshot_cap_cells = new_cap;

    return 0;
}

static void tty_snapshot_copy_cell(term_instance_t* term, int rel_row, int col) {
    if (!term) return;

    int cols = tty_snapshot_term.cols;
    if (cols <= 0) cols = TERM_W;

    int view_rows = tty_snapshot_term.view_rows;
    if (view_rows <= 0) view_rows = TERM_H;

    if (rel_row < 0 || rel_row >= view_rows) return;
    if (col < 0) col = 0;
    if (col >= cols) col = cols - 1;

    size_t dst = (size_t)rel_row * (size_t)cols + (size_t)col;
    if (dst >= tty_snapshot_cap_cells) return;

    int src_row = term->view_row + rel_row;
    if (src_row < 0 || src_row >= term->history_rows) {
        tty_snapshot_buf[dst] = ' ';
        tty_snapshot_fg[dst] = term->curr_fg;
        tty_snapshot_bg[dst] = term->curr_bg;
        return;
    }

    size_t src = (size_t)src_row * (size_t)cols + (size_t)col;

    tty_snapshot_buf[dst] = term->buffer[src];
    tty_snapshot_fg[dst] = term->fg_colors[src];
    tty_snapshot_bg[dst] = term->bg_colors[src];
}

static int tty_snapshot_reserve_rows(int rows) {
    if (rows <= tty_snapshot_cap_rows) return 0;

    int new_cap = tty_snapshot_cap_rows ? tty_snapshot_cap_rows : 128;
    while (new_cap < rows) {
        int next = new_cap << 1;
        if (next <= new_cap) {
            new_cap = rows;
            break;
        }
        new_cap = next;
    }

    uint8_t* ndr = (uint8_t*)kmalloc((size_t)new_cap ? (size_t)new_cap : 1u);
    int* ndx1 = (int*)kmalloc(((size_t)new_cap ? (size_t)new_cap : 1u) * sizeof(int));
    int* ndx2 = (int*)kmalloc(((size_t)new_cap ? (size_t)new_cap : 1u) * sizeof(int));
    if (!ndr || !ndx1 || !ndx2) {
        if (ndr) kfree(ndr);
        if (ndx1) kfree(ndx1);
        if (ndx2) kfree(ndx2);
        return -1;
    }

    if (tty_snapshot_dirty_rows) kfree(tty_snapshot_dirty_rows);
    if (tty_snapshot_dirty_x1) kfree(tty_snapshot_dirty_x1);
    if (tty_snapshot_dirty_x2) kfree(tty_snapshot_dirty_x2);

    tty_snapshot_dirty_rows = ndr;
    tty_snapshot_dirty_x1 = ndx1;
    tty_snapshot_dirty_x2 = ndx2;
    tty_snapshot_cap_rows = new_cap;

    return 0;
}

static int tty_snapshot_take(term_instance_t* term, uint32_t* out_bg, int* out_full_redraw) {
    if (!term) return -1;

    if (out_full_redraw) *out_full_redraw = 0;

    int cols = term->cols;
    if (cols <= 0) cols = TERM_W;

    int view_rows = term->view_rows;
    if (view_rows <= 0) view_rows = TERM_H;

    if (tty_snapshot_reserve_rows(view_rows) != 0) return -1;

    size_t cells = (size_t)cols * (size_t)view_rows;
    if (tty_snapshot_reserve(cells) != 0) return -1;

    if (out_bg) *out_bg = term->curr_bg;

    tty_snapshot_term.cols = cols;
    tty_snapshot_term.view_rows = view_rows;
    tty_snapshot_term.view_row = 0;
    tty_snapshot_term.history_rows = view_rows;
    tty_snapshot_term.history_cap_rows = view_rows;

    tty_snapshot_term.curr_fg = term->curr_fg;
    tty_snapshot_term.curr_bg = term->curr_bg;
    tty_snapshot_term.def_fg = term->def_fg;
    tty_snapshot_term.def_bg = term->def_bg;

    tty_snapshot_term.row = term->row - term->view_row;
    tty_snapshot_term.col = term->col;
    tty_snapshot_term.max_row = view_rows - 1;

    tty_snapshot_term.buffer = tty_snapshot_buf;
    tty_snapshot_term.fg_colors = tty_snapshot_fg;
    tty_snapshot_term.bg_colors = tty_snapshot_bg;

    tty_snapshot_term.dirty_rows = tty_snapshot_dirty_rows;
    tty_snapshot_term.dirty_x1 = tty_snapshot_dirty_x1;
    tty_snapshot_term.dirty_x2 = tty_snapshot_dirty_x2;
    tty_snapshot_term.full_redraw = 0;

    for (int y = 0; y < view_rows; y++) {
        tty_snapshot_dirty_rows[y] = 0;
        tty_snapshot_dirty_x1[y] = cols;
        tty_snapshot_dirty_x2[y] = -1;
    }

    int full_redraw = 0;
    int n = term_dirty_extract_visible(
        term,
        tty_snapshot_dirty_rows,
        tty_snapshot_dirty_x1,
        tty_snapshot_dirty_x2,
        tty_snapshot_cap_rows,
        &full_redraw
    );

    if (out_full_redraw) *out_full_redraw = full_redraw;
    tty_snapshot_term.full_redraw = full_redraw;

    uint32_t fg_def = term->curr_fg;
    uint32_t bg_def = term->curr_bg;
    int src_view_row = term->view_row;
    int src_history_rows = term->history_rows;

    for (int y = 0; y < n; y++) {
        if (!tty_snapshot_dirty_rows[y]) continue;

        int x0 = tty_snapshot_dirty_x1[y];
        int x1 = tty_snapshot_dirty_x2[y];
        if (x0 < 0) x0 = 0;
        if (x1 > cols) x1 = cols;
        if (x0 >= x1) continue;

        int src_row = src_view_row + y;
        size_t dst = (size_t)y * (size_t)cols;

        if (src_row < 0 || src_row >= src_history_rows) {
            for (int x = x0; x < x1; x++) {
                size_t i = dst + (size_t)x;
                tty_snapshot_buf[i] = ' ';
                tty_snapshot_fg[i] = fg_def;
                tty_snapshot_bg[i] = bg_def;
            }
            continue;
        }

        size_t src = (size_t)src_row * (size_t)cols;

        memcpy(tty_snapshot_buf + dst + (size_t)x0, term->buffer + src + (size_t)x0, (size_t)(x1 - x0));
        memcpy(tty_snapshot_fg + dst + (size_t)x0, term->fg_colors + src + (size_t)x0, (size_t)(x1 - x0) * sizeof(uint32_t));
        memcpy(tty_snapshot_bg + dst + (size_t)x0, term->bg_colors + src + (size_t)x0, (size_t)(x1 - x0) * sizeof(uint32_t));
    }

    return 0;
}

void tty_set_terminal(term_instance_t* term) {
    uint32_t flags = spinlock_acquire_safe(&tty_lock);
    tty_term = term;
    spinlock_release_safe(&tty_lock, flags);
}

void tty_term_apply_default_size(term_instance_t* term) {
    if (!term) return;

    int cols = (int)(fb_width / 8u);
    int view_rows = (int)(fb_height / 16u);

    if (cols < 1) cols = 1;
    if (view_rows < 1) view_rows = 1;

    term->cols = cols;
    term->view_rows = view_rows;
}

void tty_term_print_locked(term_instance_t* term, const char* text) {
    if (!term || !text) return;

    spinlock_acquire(&term->lock);
    term_print(term, text);
    spinlock_release(&term->lock);
}

static void tty_render_once(term_instance_t* term) {
    if (!term) return;

    vga_set_target(0, 0, 0);

    uint32_t bg = term->curr_bg;
    vga_draw_rect(0, 0, (int)fb_width, (int)fb_height, bg);

    vga_render_terminal_instance(term, 0, 0);

    int cols = term->cols;
    if (cols <= 0) cols = TERM_W;

    int view_rows = term->view_rows;
    if (view_rows <= 0) view_rows = TERM_H;

    int rel_row = term->row - term->view_row;
    if (rel_row >= 0 && rel_row < view_rows) {
        int cx = term->col;
        if (cx < 0) cx = 0;
        if (cx >= cols) cx = cols - 1;
        vga_draw_rect(cx * 8, rel_row * 16 + 14, 8, 2, COLOR_LIGHT_GREEN);
    }

    vga_mark_dirty(0, 0, (int)fb_width, (int)fb_height);
    vga_flip_dirty();
    vga_reset_dirty();
}

static void tty_render_fallback(void) {
    vga_set_target(0, 0, 0);
    vga_draw_rect(0, 0, (int)fb_width, (int)fb_height, 0x000000);
    vga_print_at("TTY: waiting for shell...", 16, 16, COLOR_LIGHT_GREY);
    vga_mark_dirty(0, 0, (int)fb_width, (int)fb_height);
    vga_flip_dirty();
    vga_reset_dirty();
}

void tty_task(void* arg) {
    (void)arg;

    spinlock_init(&tty_lock);

    uint64_t last_seq = 0;
    uint64_t last_view_seq = 0;
    int last_cursor_row = -1;
    int last_cursor_col = -1;

    while (1) {
        if (!fb_kernel_can_render()) {
            proc_usleep(10000);
            continue;
        }

        uint32_t flags = spinlock_acquire_safe(&tty_lock);
        term_instance_t* term = tty_term;
        spinlock_release_safe(&tty_lock, flags);

        if (term) {
            uint32_t bg = 0;
            int full_redraw = 0;

            spinlock_acquire(&term->lock);

            uint64_t cur_seq = term->seq;
            uint64_t cur_view_seq = term->view_seq;

            if (cur_seq == last_seq && cur_view_seq == last_view_seq) {
                spinlock_release(&term->lock);
                proc_usleep(10000);
                continue;
            }

            int ok = tty_snapshot_take(term, &bg, &full_redraw);

            int cur_row = tty_snapshot_term.row;
            int cur_col = tty_snapshot_term.col;

            if (cur_row != last_cursor_row || cur_col != last_cursor_col) {
                int cols = tty_snapshot_term.cols;
                if (cols <= 0) cols = TERM_W;
                int view_rows = tty_snapshot_term.view_rows;
                if (view_rows <= 0) view_rows = TERM_H;

                if (last_cursor_row >= 0 && last_cursor_row < view_rows) {
                    int x = last_cursor_col;
                    if (x < 0) x = 0;
                    if (x >= cols) x = cols - 1;
                    tty_snapshot_dirty_rows[last_cursor_row] = 1;
                    if (tty_snapshot_dirty_x1[last_cursor_row] > x) tty_snapshot_dirty_x1[last_cursor_row] = x;
                    if (tty_snapshot_dirty_x2[last_cursor_row] < x + 1) tty_snapshot_dirty_x2[last_cursor_row] = x + 1;

                    tty_snapshot_copy_cell(term, last_cursor_row, x);
                }

                if (cur_row >= 0 && cur_row < view_rows) {
                    int x = cur_col;
                    if (x < 0) x = 0;
                    if (x >= cols) x = cols - 1;
                    tty_snapshot_dirty_rows[cur_row] = 1;
                    if (tty_snapshot_dirty_x1[cur_row] > x) tty_snapshot_dirty_x1[cur_row] = x;
                    if (tty_snapshot_dirty_x2[cur_row] < x + 1) tty_snapshot_dirty_x2[cur_row] = x + 1;

                    tty_snapshot_copy_cell(term, cur_row, x);
                }
            }

            spinlock_release(&term->lock);

            if (ok == 0) {
                vga_set_target(0, 0, 0);

                int cols = tty_snapshot_term.cols;
                if (cols <= 0) cols = TERM_W;

                int view_rows = tty_snapshot_term.view_rows;
                if (view_rows <= 0) view_rows = TERM_H;

                int term_x = 0;
                int term_y = 0;
                int term_w = cols * 8;
                int term_h = view_rows * 16;
                if (term_w > (int)fb_width) term_w = (int)fb_width;
                if (term_h > (int)fb_height) term_h = (int)fb_height;

                int bb_x1 = cols;
                int bb_y1 = view_rows;
                int bb_x2 = -1;
                int bb_y2 = -1;

                for (int y = 0; y < view_rows; y++) {
                    if (!tty_snapshot_dirty_rows[y]) continue;

                    int x0 = tty_snapshot_dirty_x1[y];
                    int x1 = tty_snapshot_dirty_x2[y];
                    if (x0 < 0) x0 = 0;
                    if (x1 > cols) x1 = cols;
                    if (x0 >= x1) continue;

                    if (x0 < bb_x1) bb_x1 = x0;
                    if (y < bb_y1) bb_y1 = y;
                    if (x1 > bb_x2) bb_x2 = x1;
                    if (y + 1 > bb_y2) bb_y2 = y + 1;
                }

                if (full_redraw) {
                    vga_draw_rect(term_x, term_y, term_w, term_h, bg);
                    vga_render_terminal_instance(&tty_snapshot_term, term_x, term_y);
                    vga_mark_dirty(term_x, term_y, term_w, term_h);
                } else if (bb_x1 <= bb_x2 && bb_y1 <= bb_y2) {
                    vga_render_terminal_instance(&tty_snapshot_term, term_x, term_y);
                    vga_mark_dirty(term_x + bb_x1 * 8, term_y + bb_y1 * 16, (bb_x2 - bb_x1) * 8, (bb_y2 - bb_y1) * 16);
                }

                int rel_row = tty_snapshot_term.row;
                if (rel_row >= 0 && rel_row < view_rows) {
                    int cx = tty_snapshot_term.col;
                    if (cx < 0) cx = 0;
                    if (cx >= cols) cx = cols - 1;
                    vga_draw_rect(term_x + cx * 8, term_y + rel_row * 16 + 14, 8, 2, COLOR_LIGHT_GREEN);
                    vga_mark_dirty(term_x + cx * 8, term_y + rel_row * 16 + 14, 8, 2);
                }

                vga_flip_dirty();
                vga_reset_dirty();
            }

            last_seq = cur_seq;
            last_view_seq = cur_view_seq;
            last_cursor_row = cur_row;
            last_cursor_col = cur_col;
        } else {
            tty_render_fallback();
        }

        proc_usleep(10000);
    }
}
