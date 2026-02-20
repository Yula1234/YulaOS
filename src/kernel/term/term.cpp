#if 0

static inline void term_bump_view_seq(TermState* term) {
    if (!term) {
        return;
    }

    term->view_seq++;
    if (term->view_seq == 0) {
        term->view_seq = 1;
    }
}

static inline void term_dirty_reset_row(TermState* term, int row, int cols) {
    if (!term || !term->dirty_rows || !term->dirty_x1 || !term->dirty_x2) {
        return;
    }

    if (row < 0 || row >= term->history_cap_rows) {
        return;
    }

    term->dirty_rows[row] = 0;
    term->dirty_x1[row] = cols;
    term->dirty_x2[row] = -1;
}

static inline void term_dirty_mark_range(TermState* term, int row, int x0, int x1) {
    if (!term) {
        return;
    }

    int cols = term->cols;
    if (cols <= 0) {
        cols = kDefaultCols;
    }

    if (row < 0) {
        return;
    }

    if (x0 < 0) {
        x0 = 0;
    }

    if (x1 > cols) {
        x1 = cols;
    }

    if (x0 >= x1) {
        return;
    }

    if (!term->dirty_rows || !term->dirty_x1 || !term->dirty_x2) {
        term->full_redraw = 1;
        return;
    }

    if (row >= term->history_cap_rows) {
        return;
    }

    term->dirty_rows[row] = 1;

    if (term->dirty_x1[row] > x0) {
        term->dirty_x1[row] = x0;
    }

    if (term->dirty_x2[row] < x1) {
        term->dirty_x2[row] = x1;
    }
}

static void term_mark_all_dirty(TermState* term) {
    if (!term) {
        return;
    }

    int cols = term->cols;
    if (cols <= 0) {
        cols = kDefaultCols;
    }

    int rows = term->history_rows;
    if (rows < 1) {
        rows = 1;
    }

    if (rows > term->history_cap_rows) {
        rows = term->history_cap_rows;
    }

    if (!term->dirty_rows || !term->dirty_x1 || !term->dirty_x2) {
        term->full_redraw = 1;
        return;
    }

    for (int r = 0; r < rows; r++) {
        term->dirty_rows[r] = 1;
        term->dirty_x1[r] = 0;
        term->dirty_x2[r] = cols;
    }

    for (int r = rows; r < term->history_cap_rows; r++) {
        term_dirty_reset_row(term, r, cols);
    }
}

static const uint32_t term_ansi_colors[8] = {
    0x000000u,
    0xAA0000u,
    0x00AA00u,
    0xAA5500u,
    0x0000AAu,
    0xAA00AAu,
    0x00AAAAu,
    0xAAAAAAu
};

static const uint32_t term_ansi_bright_colors[8] = {
    0x555555u,
    0xFF5555u,
    0x55FF55u,
    0xFFFF55u,
    0x5555FFu,
    0xFF55FFu,
    0x55FFFFu,
    0xFFFFFFu
};

static int term_ensure_rows(TermState* term, int rows_needed) {
    if (!term) {
        return -1;
    }

    if (rows_needed <= 0) {
        rows_needed = 1;
    }

    if (term->history_cap_rows >= rows_needed) {
        return 0;
    }

    int cols = term->cols;
    if (cols <= 0) {
        cols = kDefaultCols;
    }

    int old_cap = term->history_cap_rows;
    int new_cap = (old_cap > 0) ? old_cap : 128;

    while (new_cap < rows_needed) {
        if (new_cap > (1 << 28)) {
            return -1;
        }
        new_cap *= 2;
    }

    size_t old_cells = (size_t)old_cap * (size_t)cols;
    size_t new_cells = (size_t)new_cap * (size_t)cols;

    term->buffer = (char*)krealloc(term->buffer, new_cells);
    term->fg_colors = (uint32_t*)krealloc(term->fg_colors, new_cells * sizeof(uint32_t));
    term->bg_colors = (uint32_t*)krealloc(term->bg_colors, new_cells * sizeof(uint32_t));

    term->dirty_rows = (uint8_t*)krealloc(term->dirty_rows, (size_t)new_cap);
    term->dirty_x1 = (int*)krealloc(term->dirty_x1, (size_t)new_cap * sizeof(int));
    term->dirty_x2 = (int*)krealloc(term->dirty_x2, (size_t)new_cap * sizeof(int));

    if (!term->buffer || !term->fg_colors || !term->bg_colors || !term->dirty_rows || !term->dirty_x1 || !term->dirty_x2) {
        return -1;
    }

    for (size_t i = old_cells; i < new_cells; i++) {
        term->buffer[i] = ' ';
        term->fg_colors[i] = term->curr_fg;
        term->bg_colors[i] = term->curr_bg;
    }

    for (int r = old_cap; r < new_cap; r++) {
        term->dirty_rows[r] = 1;
        term->dirty_x1[r] = 0;
        term->dirty_x2[r] = cols;
    }

    term->history_cap_rows = new_cap;
    return 0;
}

static inline uint32_t term_effective_fg(const TermState* term) {
    return (term && term->ansi_inverse) ? term->curr_bg : term->curr_fg;
}

static inline uint32_t term_effective_bg(const TermState* term) {
    return (term && term->ansi_inverse) ? term->curr_fg : term->curr_bg;
}

static void term_ansi_reset(TermState* term) {
    if (!term) {
        return;
    }

    term->esc_state = 0;
    term->csi_in_param = 0;
    term->csi_param_value = 0;
    term->csi_param_count = 0;
}

static void term_csi_push_param(TermState* term) {
    if (!term) {
        return;
    }

    if (term->csi_param_count < (int)(sizeof(term->csi_params) / sizeof(term->csi_params[0]))) {
        int v = term->csi_in_param ? term->csi_param_value : 0;
        term->csi_params[term->csi_param_count++] = v;
    }

    term->csi_param_value = 0;
    term->csi_in_param = 0;
}

static inline int term_csi_param(const TermState* term, int idx, int def) {
    if (!term || idx < 0 || idx >= term->csi_param_count) {
        return def;
    }

    int v = term->csi_params[idx];
    return (v == 0) ? def : v;
}

static void term_set_cursor(TermState* term, int row, int col) {
    if (!term) {
        return;
    }

    int cols = term->cols;
    if (cols <= 0) {
        cols = kDefaultCols;
    }

    if (row < 0) {
        row = 0;
    }

    if (col < 0) {
        col = 0;
    }

    if (col >= cols) {
        col = cols - 1;
    }

    if (term_ensure_rows(term, row + 1) != 0) {
        return;
    }

    term->row = row;
    term->col = col;

    if (term->row >= term->history_rows) {
        term->history_rows = term->row + 1;
    }

    if (term->row > term->max_row) {
        term->max_row = term->row;
    }

    term_bump_view_seq(term);
}

static void term_clear_row_range(TermState* term, int row, int x0, int x1) {
    if (!term) {
        return;
    }

    int cols = term->cols;
    if (cols <= 0) {
        cols = kDefaultCols;
    }

    if (row < 0) {
        return;
    }

    if (x0 < 0) {
        x0 = 0;
    }

    if (x1 > cols) {
        x1 = cols;
    }

    if (x0 >= x1) {
        return;
    }

    if (term_ensure_rows(term, row + 1) != 0) {
        return;
    }

    size_t base = (size_t)row * (size_t)cols + (size_t)x0;
    uint32_t fg = term_effective_fg(term);
    uint32_t bg = term_effective_bg(term);

    for (int x = x0; x < x1; x++) {
        term->buffer[base] = ' ';
        term->fg_colors[base] = fg;
        term->bg_colors[base] = bg;
        base++;
    }

    if (row >= term->history_rows) {
        term->history_rows = row + 1;
    }

    if (row > term->max_row) {
        term->max_row = row;
    }

    term_dirty_mark_range(term, row, x0, x1);
    term_bump_seq(term);
}

static void term_clear_all(TermState* term) {
    if (!term) {
        return;
    }

    int rows = term->history_rows;
    if (rows < 1) {
        rows = 1;
    }

    for (int r = 0; r < rows; r++) {
        term_clear_row_range(term, r, 0, term->cols);
    }

    term->col = 0;
    term->row = 0;
    term->view_row = 0;
    term->max_row = 0;
    term->history_rows = 1;

    term->full_redraw = 1;
    term_mark_all_dirty(term);
    term_bump_view_seq(term);
}

static void term_apply_sgr(TermState* term) {
    if (!term) {
        return;
    }

    if (term->csi_param_count == 0) {
        term->curr_fg = term->def_fg;
        term->curr_bg = term->def_bg;
        term->ansi_bright = 0;
        term->ansi_inverse = 0;
        return;
    }

    for (int i = 0; i < term->csi_param_count; i++) {
        int p = term->csi_params[i];

        if (p == 0) {
            term->curr_fg = term->def_fg;
            term->curr_bg = term->def_bg;
            term->ansi_bright = 0;
            term->ansi_inverse = 0;
        } else if (p == 1) {
            term->ansi_bright = 1;
        } else if (p == 22) {
            term->ansi_bright = 0;
        } else if (p == 7) {
            term->ansi_inverse = 1;
        } else if (p == 27) {
            term->ansi_inverse = 0;
        } else if (p == 39) {
            term->curr_fg = term->def_fg;
        } else if (p == 49) {
            term->curr_bg = term->def_bg;
        } else if (p >= 30 && p <= 37) {
            int idx = p - 30;
            term->curr_fg = term->ansi_bright ? term_ansi_bright_colors[idx] : term_ansi_colors[idx];
        } else if (p >= 90 && p <= 97) {
            int idx = p - 90;
            term->curr_fg = term_ansi_bright_colors[idx];
        } else if (p >= 40 && p <= 47) {
            int idx = p - 40;
            term->curr_bg = term->ansi_bright ? term_ansi_bright_colors[idx] : term_ansi_colors[idx];
        } else if (p >= 100 && p <= 107) {
            int idx = p - 100;
            term->curr_bg = term_ansi_bright_colors[idx];
        }
    }
}

static void term_handle_csi(TermState* term, char cmd) {
    if (!term) {
        return;
    }

    if (cmd == 'A') {
        int n = term_csi_param(term, 0, 1);
        term_set_cursor(term, term->row - n, term->col);
    } else if (cmd == 'B') {
        int n = term_csi_param(term, 0, 1);
        term_set_cursor(term, term->row + n, term->col);
    } else if (cmd == 'C') {
        int n = term_csi_param(term, 0, 1);
        term_set_cursor(term, term->row, term->col + n);
    } else if (cmd == 'D') {
        int n = term_csi_param(term, 0, 1);
        term_set_cursor(term, term->row, term->col - n);
    } else if (cmd == 'H' || cmd == 'f') {
        int r = term_csi_param(term, 0, 1) - 1;
        int c = term_csi_param(term, 1, 1) - 1;
        term_set_cursor(term, r, c);
    } else if (cmd == 'J') {
        int mode = term->csi_param_count > 0 ? term->csi_params[0] : 0;

        if (mode == 2) {
            term_clear_all(term);
        } else if (mode == 0) {
            term_clear_row_range(term, term->row, term->col, term->cols);

            for (int r = term->row + 1; r < term->view_row + term->view_rows; r++) {
                term_clear_row_range(term, r, 0, term->cols);
            }
        } else if (mode == 1) {
            for (int r = term->view_row; r < term->row; r++) {
                term_clear_row_range(term, r, 0, term->cols);
            }

            term_clear_row_range(term, term->row, 0, term->col + 1);
        }
    } else if (cmd == 'K') {
        int mode = term->csi_param_count > 0 ? term->csi_params[0] : 0;

        if (mode == 0) {
            term_clear_row_range(term, term->row, term->col, term->cols);
        } else if (mode == 1) {
            term_clear_row_range(term, term->row, 0, term->col + 1);
        } else if (mode == 2) {
            term_clear_row_range(term, term->row, 0, term->cols);
        }
    } else if (cmd == 'm') {
        term_apply_sgr(term);
    } else if (cmd == 's') {
        term->saved_row = term->row;
        term->saved_col = term->col;
    } else if (cmd == 'u') {
        term_set_cursor(term, term->saved_row, term->saved_col);
    }
}

void term_init(TermState* term) {
    if (!term) {
        return;
    }

    spinlock_init(&term->lock);

    term->seq = 1;
    term->view_seq = 1;

    term->max_row = 0;
    term->history_rows = 1;

    if (term->curr_fg == 0) {
        term->curr_fg = 0xD4D4D4u;
    }

    if (term->curr_bg == 0) {
        term->curr_bg = 0x141414u;
    }

    term->def_fg = term->curr_fg;
    term->def_bg = term->curr_bg;

    if (term->cols <= 0) {
        term->cols = kDefaultCols;
    }

    if (term->view_rows <= 0) {
        term->view_rows = kDefaultRows;
    }

    term->buffer = 0;
    term->fg_colors = 0;
    term->bg_colors = 0;

    term->dirty_rows = 0;
    term->dirty_x1 = 0;
    term->dirty_x2 = 0;
    term->full_redraw = 1;

    (void)term_ensure_rows(term, 1);

    term_mark_all_dirty(term);

    term->col = 0;
    term->row = 0;
    term->view_row = 0;
    term->max_row = 0;

    term->saved_col = 0;
    term->saved_row = 0;

    term->esc_state = 0;
    term->csi_in_param = 0;
    term->csi_param_value = 0;
    term->csi_param_count = 0;

    term->ansi_bright = 0;
    term->ansi_inverse = 0;
}

void term_destroy(TermState* term) {
    if (!term) {
        return;
    }

    if (term->buffer) {
        kfree(term->buffer);
    }

    if (term->fg_colors) {
        kfree(term->fg_colors);
    }

    if (term->bg_colors) {
        kfree(term->bg_colors);
    }

    if (term->dirty_rows) {
        kfree(term->dirty_rows);
    }

    if (term->dirty_x1) {
        kfree(term->dirty_x1);
    }

    if (term->dirty_x2) {
        kfree(term->dirty_x2);
    }

    term->buffer = 0;
    term->fg_colors = 0;
    term->bg_colors = 0;

    term->dirty_rows = 0;
    term->dirty_x1 = 0;
    term->dirty_x2 = 0;

    term->history_cap_rows = 0;
    term->history_rows = 0;
}

void term_clear_row(TermState* term, int row) {
    if (!term || row < 0) {
        return;
    }

    if (term_ensure_rows(term, row + 1) != 0) {
        return;
    }

    int cols = term->cols;
    if (cols <= 0) {
        cols = kDefaultCols;
    }

    size_t start = (size_t)row * (size_t)cols;

    for (int i = 0; i < cols; i++) {
        term->buffer[start + (size_t)i] = ' ';
        term->fg_colors[start + (size_t)i] = term->curr_fg;
        term->bg_colors[start + (size_t)i] = term->curr_bg;
    }

    if (row >= term->history_rows) {
        term->history_rows = row + 1;
    }

    term_dirty_mark_range(term, row, 0, cols);
    term_bump_seq(term);
}

void term_get_cell(TermState* term, int row, int col, char* out_ch, uint32_t* out_fg, uint32_t* out_bg) {
    if (out_ch) {
        *out_ch = ' ';
    }

    if (out_fg) {
        *out_fg = term ? term->curr_fg : 0;
    }

    if (out_bg) {
        *out_bg = term ? term->curr_bg : 0;
    }

    int cols = term ? term->cols : 0;
    if (cols <= 0) {
        cols = kDefaultCols;
    }

    if (!term || row < 0 || col < 0 || col >= cols) {
        return;
    }

    if (row >= term->history_rows) {
        return;
    }

    size_t idx = (size_t)row * (size_t)cols + (size_t)col;

    if (out_ch) {
        *out_ch = term->buffer[idx];
    }

    if (out_fg) {
        *out_fg = term->fg_colors[idx];
    }

    if (out_bg) {
        *out_bg = term->bg_colors[idx];
    }
}

void term_set_cell(TermState* term, int row, int col, char ch, uint32_t fg, uint32_t bg) {
    if (!term) {
        return;
    }

    int cols = term->cols;
    if (cols <= 0) {
        cols = kDefaultCols;
    }

    if (row < 0 || col < 0 || col >= cols) {
        return;
    }

    if (term_ensure_rows(term, row + 1) != 0) {
        return;
    }

    size_t idx = (size_t)row * (size_t)cols + (size_t)col;

    if (term->buffer[idx] == ch && term->fg_colors[idx] == fg && term->bg_colors[idx] == bg) {
        if (row >= term->history_rows) {
            term->history_rows = row + 1;
        }

        if (row > term->max_row) {
            term->max_row = row;
        }

        return;
    }

    term->buffer[idx] = ch;
    term->fg_colors[idx] = fg;
    term->bg_colors[idx] = bg;

    if (row >= term->history_rows) {
        term->history_rows = row + 1;
    }

    if (row > term->max_row) {
        term->max_row = row;
    }

    term_dirty_mark_range(term, row, col, col + 1);
    term_bump_seq(term);
}

void term_putc(TermState* term, char c) {
    if (!term) {
        return;
    }

    int cols = term->cols;
    if (cols <= 0) {
        cols = kDefaultCols;
    }

    int view_rows = term->view_rows;
    if (view_rows <= 0) {
        view_rows = kDefaultRows;
    }

    if ((uint8_t)c == 0x0Cu) {
        term->col = 0;
        term->row = 0;
        term->view_row = 0;
        term->max_row = 0;
        term->history_rows = 1;

        term_clear_row(term, 0);

        term->full_redraw = 1;
        term_mark_all_dirty(term);
        term_bump_view_seq(term);
        return;
    }

    if (c == '\r') {
        term->col = 0;
        term_bump_view_seq(term);
        return;
    }

    if (c == '\n') {
        if (term_ensure_rows(term, term->row + 1) != 0) {
            return;
        }

        int idx = term->row * cols + term->col;
        uint32_t fg = term_effective_fg(term);
        uint32_t bg = term_effective_bg(term);
        int remaining = cols - term->col;

        for (int k = 0; k < remaining; k++) {
            term->bg_colors[idx + k] = bg;
            term->fg_colors[idx + k] = fg;
            term->buffer[idx + k] = ' ';
        }

        term_dirty_mark_range(term, term->row, term->col, cols);

        term->col = 0;
        term->row++;

        term_clear_row(term, term->row);
    } else if (c == '\b') {
        if (term->col > 0) {
            term->col--;
        }

        if (term_ensure_rows(term, term->row + 1) != 0) {
            return;
        }

        int idx = term->row * cols + term->col;
        term->buffer[idx] = ' ';
        term->fg_colors[idx] = term_effective_fg(term);
        term->bg_colors[idx] = term_effective_bg(term);

        term_dirty_mark_range(term, term->row, term->col, term->col + 1);
    } else {
        if (term_ensure_rows(term, term->row + 1) != 0) {
            return;
        }

        int idx = term->row * cols + term->col;
        term->buffer[idx] = c;
        term->fg_colors[idx] = term_effective_fg(term);
        term->bg_colors[idx] = term_effective_bg(term);

        term_dirty_mark_range(term, term->row, term->col, term->col + 1);
        term->col++;
    }

    if (term->col >= cols) {
        term->col = 0;
        term->row++;
        term_clear_row(term, term->row);
    }

    if (term->row >= term->history_rows) {
        term->history_rows = term->row + 1;
    }

    if (term->row > term->max_row) {
        term->max_row = term->row;
    }

    int old_view_row = term->view_row;

    int at_bottom = (term->view_row + view_rows) >= term->row;
    if (at_bottom) {
        if (term->row >= view_rows) {
            term->view_row = term->row - view_rows + 1;
        } else {
            term->view_row = 0;
        }
    }

    term_bump_seq(term);

    if (term->view_row != old_view_row) {
        term_invalidate_view(term);
    } else {
        term_bump_view_seq(term);
    }
}

void term_invalidate_view(TermState* term) {
    if (!term) {
        return;
    }

    term->full_redraw = 1;
    term_mark_all_dirty(term);
    term_bump_view_seq(term);
}

int term_dirty_extract_visible(TermState* term, uint8_t* out_rows, int* out_x1, int* out_x2, int out_rows_cap, int* out_full_redraw) {
    if (out_full_redraw) {
        *out_full_redraw = 0;
    }

    if (!term || !out_rows || !out_x1 || !out_x2 || out_rows_cap <= 0) {
        return 0;
    }

    int cols = term->cols;
    if (cols <= 0) {
        cols = kDefaultCols;
    }

    int view_rows = term->view_rows;
    if (view_rows <= 0) {
        view_rows = kDefaultRows;
    }

    int n = (view_rows < out_rows_cap) ? view_rows : out_rows_cap;

    int full = term->full_redraw;
    if (out_full_redraw) {
        *out_full_redraw = full ? 1 : 0;
    }

    if (full || !term->dirty_rows || !term->dirty_x1 || !term->dirty_x2) {
        for (int y = 0; y < n; y++) {
            out_rows[y] = 1;
            out_x1[y] = 0;
            out_x2[y] = cols;
        }

        term->full_redraw = 0;

        if (term->dirty_rows && term->dirty_x1 && term->dirty_x2) {
            int rows = term->history_rows;

            if (rows < 1) {
                rows = 1;
            }

            if (rows > term->history_cap_rows) {
                rows = term->history_cap_rows;
            }

            for (int r = 0; r < rows; r++) {
                term_dirty_reset_row(term, r, cols);
            }
        }

        return n;
    }

    for (int y = 0; y < n; y++) {
        int src_row = term->view_row + y;

        if (src_row < 0 || src_row >= term->history_cap_rows) {
            out_rows[y] = 0;
            out_x1[y] = cols;
            out_x2[y] = -1;
            continue;
        }

        if (!term->dirty_rows[src_row]) {
            out_rows[y] = 0;
            out_x1[y] = cols;
            out_x2[y] = -1;
            continue;
        }

        out_rows[y] = 1;
        out_x1[y] = term->dirty_x1[src_row];
        out_x2[y] = term->dirty_x2[src_row];

        if (out_x1[y] < 0) {
            out_x1[y] = 0;
        }

        if (out_x2[y] > cols) {
            out_x2[y] = cols;
        }

        term_dirty_reset_row(term, src_row, cols);
    }

    return n;
}

void term_write(TermState* term, const char* buf, uint32_t len) {
    if (!term || !buf || len == 0) {
        return;
    }

    uint32_t i = 0;

    while (i < len) {
        char c = buf[i++];

        if (term->esc_state == 0) {
            if ((uint8_t)c == 0x1Bu) {
                term->esc_state = 1;
                continue;
            }

            if (c == '\r' || c == '\n' || c == '\b' || (uint8_t)c == 0x0Cu) {
                term_putc(term, c);
                continue;
            }

            term_putc(term, c);
            continue;
        }

        if (term->esc_state == 1) {
            if (c == '[') {
                term->esc_state = 2;
                term->csi_param_count = 0;
                term->csi_param_value = 0;
                term->csi_in_param = 0;
                continue;
            }

            if (c == '7') {
                term->saved_row = term->row;
                term->saved_col = term->col;
                term_ansi_reset(term);
                continue;
            }

            if (c == '8') {
                term_set_cursor(term, term->saved_row, term->saved_col);
                term_ansi_reset(term);
                continue;
            }

            term_ansi_reset(term);
            continue;
        }

        if (term->esc_state == 2) {
            if (c >= '0' && c <= '9') {
                term->csi_in_param = 1;
                term->csi_param_value = term->csi_param_value * 10 + (int)(c - '0');

                if (term->csi_param_value > 9999) {
                    term->csi_param_value = 9999;
                }

                continue;
            }

            if (c == ';') {
                term_csi_push_param(term);
                continue;
            }

            if (term->csi_in_param || term->csi_param_count > 0) {
                term_csi_push_param(term);
            }

            term_handle_csi(term, c);
            term_ansi_reset(term);
        }
    }
}

void term_print(TermState* term, const char* s) {
    if (!term || !s) {
        return;
    }

    uint32_t len = (uint32_t)strlen(s);
    term_write(term, s, len);
}

void term_print_u32(TermState* term, uint32_t n) {
    if (!term) {
        return;
    }

    char tmp[11];
    uint32_t pos = 0;

    if (n == 0) {
        tmp[pos++] = '0';
    } else {
        char rev[11];
        uint32_t rpos = 0;

        while (n > 0 && rpos < sizeof(rev)) {
            rev[rpos++] = (char)('0' + (n % 10u));
            n /= 10u;
        }

        while (rpos > 0) {
            tmp[pos++] = rev[--rpos];
        }
    }

    tmp[pos] = '\0';
    term_print(term, tmp);
}

void term_reflow(TermState* term, int new_cols) {
    if (!term) {
        return;
    }

    if (new_cols <= 0) {
        new_cols = 1;
    }

    int old_cols = term->cols;
    if (old_cols <= 0) {
        old_cols = kDefaultCols;
    }

    if (new_cols == old_cols) {
        term->cols = new_cols;
        return;
    }

    int old_last_row = term->max_row;

    if (old_last_row < 0) {
        old_last_row = 0;
    }

    if (old_last_row >= term->history_rows) {
        old_last_row = term->history_rows - 1;
    }

    if (old_last_row < 0) {
        old_last_row = 0;
    }

    size_t worst = ((size_t)(old_last_row + 1) * (size_t)old_cols) + (size_t)(old_last_row + 1);
    int cap_rows = (int)(worst / (size_t)new_cols) + 2;

    if (cap_rows < 1) {
        cap_rows = 1;
    }

    size_t cells = (size_t)cap_rows * (size_t)new_cols;

    char* nb = (char*)kmalloc(cells ? cells : 1);
    uint32_t* nfg = (uint32_t*)kmalloc((cells ? cells : 1) * sizeof(uint32_t));
    uint32_t* nbg = (uint32_t*)kmalloc((cells ? cells : 1) * sizeof(uint32_t));
    uint8_t* ndr = (uint8_t*)kmalloc((size_t)cap_rows ? (size_t)cap_rows : 1u);
    int* ndx1 = (int*)kmalloc(((size_t)cap_rows ? (size_t)cap_rows : 1u) * sizeof(int));
    int* ndx2 = (int*)kmalloc(((size_t)cap_rows ? (size_t)cap_rows : 1u) * sizeof(int));

    if (!nb || !nfg || !nbg || !ndr || !ndx1 || !ndx2) {
        if (nb) kfree(nb);
        if (nfg) kfree(nfg);
        if (nbg) kfree(nbg);
        if (ndr) kfree(ndr);
        if (ndx1) kfree(ndx1);
        if (ndx2) kfree(ndx2);
        return;
    }

    for (size_t i = 0; i < cells; i++) {
        nb[i] = ' ';
        nfg[i] = term->curr_fg;
        nbg[i] = term->curr_bg;
    }

    for (int r = 0; r < cap_rows; r++) {
        ndr[r] = 1;
        ndx1[r] = 0;
        ndx2[r] = new_cols;
    }

    int cur_row = term->row;
    int cur_col = term->col;

    if (cur_row < 0) {
        cur_row = 0;
    }

    if (cur_col < 0) {
        cur_col = 0;
    }

    if (cur_col > old_cols) {
        cur_col = old_cols;
    }

    int out_r = 0;
    int out_c = 0;

    int new_cur_r = 0;
    int new_cur_c = 0;
    int have_cur = 0;

    int new_view_r = 0;
    int have_view = 0;

    for (int r = 0; r <= old_last_row && out_r < cap_rows; r++) {
        if (!have_view && r == term->view_row) {
            new_view_r = out_r;
            have_view = 1;
        }

        int end = old_cols - 1;
        while (end >= 0 && term->buffer[(size_t)r * (size_t)old_cols + (size_t)end] == ' ') {
            end--;
        }

        int row_len = end + 1;
        if (row_len < 0) {
            row_len = 0;
        }

        int take_cur = -1;
        if (r == cur_row) {
            take_cur = cur_col;
            if (take_cur > row_len) {
                take_cur = row_len;
            }
        }

        for (int c = 0; c < row_len && out_r < cap_rows; c++) {
            if (!have_cur && r == cur_row && c == take_cur) {
                new_cur_r = out_r;
                new_cur_c = out_c;
                have_cur = 1;
            }

            size_t dst = (size_t)out_r * (size_t)new_cols + (size_t)out_c;
            size_t src = (size_t)r * (size_t)old_cols + (size_t)c;

            nb[dst] = term->buffer[src];
            nfg[dst] = term->fg_colors[src];
            nbg[dst] = term->bg_colors[src];

            if (++out_c >= new_cols) {
                out_c = 0;
                out_r++;
            }
        }

        if (!have_cur && r == cur_row && take_cur == row_len) {
            new_cur_r = out_r;
            new_cur_c = out_c;
            have_cur = 1;
        }

        int hard_nl = (r < old_last_row && end < (old_cols - 1));
        if (hard_nl) {
            out_r++;
            out_c = 0;
        }
    }

    if (out_r >= cap_rows) {
        out_r = cap_rows - 1;
        out_c = 0;
    }

    kfree(term->buffer);
    kfree(term->fg_colors);
    kfree(term->bg_colors);

    if (term->dirty_rows) {
        kfree(term->dirty_rows);
    }

    if (term->dirty_x1) {
        kfree(term->dirty_x1);
    }

    if (term->dirty_x2) {
        kfree(term->dirty_x2);
    }

    term->buffer = nb;
    term->fg_colors = nfg;
    term->bg_colors = nbg;

    term->dirty_rows = ndr;
    term->dirty_x1 = ndx1;
    term->dirty_x2 = ndx2;

    term->cols = new_cols;
    term->history_cap_rows = cap_rows;
    term->history_rows = out_r + 1;
    term->max_row = term->history_rows - 1;

    term->view_row = have_view ? new_view_r : term->view_row;

    if (term->view_row < 0) {
        term->view_row = 0;
    }

    if (term->view_row > term->max_row) {
        term->view_row = term->max_row;
    }

    term->row = have_cur ? new_cur_r : out_r;
    term->col = have_cur ? new_cur_c : out_c;

    if (term->row < 0) {
        term->row = 0;
    }

    if (term->row > term->max_row) {
        term->row = term->max_row;
    }

    if (term->col < 0) {
        term->col = 0;
    }

    if (term->col >= term->cols) {
        term->col = term->cols - 1;
    }

    term->full_redraw = 1;
    term_mark_all_dirty(term);
    term_bump_seq(term);
    term_bump_view_seq(term);
}

}

#endif
