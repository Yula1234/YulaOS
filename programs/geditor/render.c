// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#include "render.h"
#include "editor.h"
#include "gapbuf.h"
#include "lines.h"
#include "util.h"

static int check_kw(const char* s, int len, const char** list) {
    if (!s || !list) {
        return 0;
    }
    for (int i = 0; list[i]; i++) {
        const char* kw = list[i];
        int kwlen = (int)strlen(kw);
        if (kwlen != len) {
            continue;
        }
        int ok = 1;
        for (int j = 0; j < len; j++) {
            if (kw[j] != s[j]) {
                ok = 0;
                break;
            }
        }
        if (ok) {
            return 1;
        }
    }
    return 0;
}

static int check_kw_gb(int pos, int len, const char** list) {
    if (!list) {
        return 0;
    }
    for (int i = 0; list[i]; i++) {
        const char* kw = list[i];
        int kwlen = (int)strlen(kw);
        if (kwlen != len) {
            continue;
        }
        int ok = 1;
        for (int j = 0; j < len; j++) {
            if (gb_char_at(&ed.text, pos + j) != kw[j]) {
                ok = 0;
                break;
            }
        }
        if (ok) {
            return 1;
        }
    }
    return 0;
}

static int is_word_start_at(int pos, int line_start) {
    char c = gb_char_at(&ed.text, pos);
    if (!is_word_char(c)) {
        return 0;
    }
    if (pos <= line_start) {
        return 1;
    }
    return !is_word_char(gb_char_at(&ed.text, pos - 1));
}

static int word_len_at(int pos, int line_end) {
    int len = 0;
    while (pos + len < line_end && is_word_char(gb_char_at(&ed.text, pos + len))) {
        len++;
    }
    return len;
}

static void render_at(int line_y, int* col, int pos, char c, uint32_t color, int has_sel, int sel_start, int sel_end) {
    if (has_sel && pos >= sel_start && pos < sel_end) {
        draw_rect(GUTTER_W + PAD_X + (*col) * CHAR_W, line_y, CHAR_W, LINE_H, C_SELECTION);
    }
    render_char(canvas, WIN_W, WIN_H, GUTTER_W + PAD_X + (*col) * CHAR_W, line_y, c, color);
    (*col)++;
}

static void render_span(int line_y, int* col, int pos, int len, uint32_t color, int has_sel, int sel_start, int sel_end) {
    for (int i = 0; i < len; i++) {
        char c = gb_char_at(&ed.text, pos + i);
        render_at(line_y, col, pos + i, c, color, has_sel, sel_start, sel_end);
    }
}

void draw_rect(int x, int y, int w, int h, uint32_t color) {
    if (!canvas) {
        return;
    }
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > WIN_W) {
        w = WIN_W - x;
    }
    if (y + h > WIN_H) {
        h = WIN_H - y;
    }
    if (w <= 0 || h <= 0) {
        return;
    }

    uint32_t* row = canvas + y * WIN_W + x;
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            row[i] = color;
        }
        row += WIN_W;
    }
}

void render_char(uint32_t* fb, int fb_w, int fb_h, int x, int y, char c, uint32_t color) {
    draw_char(canvas, WIN_W, WIN_H, x, y, c, color);
}

void render_string(int x, int y, const char* s, uint32_t color) {
    if (!s) {
        return;
    }
    int i = 0;
    while (s[i]) {
        render_char(canvas, WIN_W, WIN_H, x + i * CHAR_W, y, s[i], color);
        i++;
    }
}

void render_editor(void) {
    draw_rect(0, 0, WIN_W, WIN_H, C_BG);
    draw_rect(0, TAB_H, GUTTER_W, WIN_H - TAB_H - STATUS_H, C_GUTTER_BG);

    if (ed.lines.count <= 0) {
        lines_rebuild(&ed.lines, &ed.text, ed.lang);
    }

    int text_len = gb_len(&ed.text);
    int line_count = ed.lines.count;
    int max_rows = (WIN_H - TAB_H - STATUS_H) / LINE_H;
    if (max_rows < 1) {
        max_rows = 1;
    }
    if (ed.scroll_y < 0) {
        ed.scroll_y = 0;
    }
    if (ed.scroll_y > line_count - 1) {
        ed.scroll_y = line_count - 1;
    }

    int line_y = TAB_H;
    for (int row = 0; row < max_rows; row++) {
        int line = ed.scroll_y + row;
        if (line >= line_count) {
            break;
        }
        int line_start = ed.lines.starts[line];
        int line_end = (line + 1 < line_count) ? (ed.lines.starts[line + 1] - 1) : text_len;
        if (line_end < line_start) {
            line_end = line_start;
        }

        if (line_start <= ed.cursor && ed.cursor <= line_end) {
            draw_rect(GUTTER_W, line_y, WIN_W - GUTTER_W, LINE_H, C_ACTIVE_LINE);
        }

        int ln = line + 1;
        char num_buf[16];
        fmt_int(ln, num_buf);
        int num_len = (int)strlen(num_buf);
        int num_x = GUTTER_W - 8 - num_len * CHAR_W;
        render_string(num_x, line_y, num_buf, C_GUTTER_FG);

        const int has_sel = (ed.sel_bound != -1 && ed.cursor != ed.sel_bound);
        int sel_start = 0;
        int sel_end = 0;
        if (has_sel) {
            sel_start = min(ed.sel_bound, ed.cursor);
            sel_end = max(ed.sel_bound, ed.cursor);
        }

        int col = 0;
        int in_string = 0;
        int in_char = 0;
        int in_line_comment = 0;
        int in_block_comment = (ed.lang == LANG_C && ed.lines.c_block) ? ed.lines.c_block[line] : 0;

        int pos = line_start;
        while (pos < line_end) {
            char c = gb_char_at(&ed.text, pos);

            uint32_t color = C_TEXT;
            int token_len = 0;
            int render_token = 0;

            if (ed.lang == LANG_C) {
                if (in_line_comment) {
                    color = C_SYN_COMMENT;
                } else if (in_block_comment) {
                    color = C_SYN_COMMENT;
                    if (c == '*' && pos + 1 < line_end && gb_char_at(&ed.text, pos + 1) == '/') {
                        render_char(canvas, WIN_W, WIN_H, GUTTER_W + PAD_X + col * CHAR_W, line_y, c, color);
                        col++;
                        pos++;
                        render_char(canvas, WIN_W, WIN_H, GUTTER_W + PAD_X + col * CHAR_W, line_y, '/', color);
                        col++;
                        pos++;
                        in_block_comment = 0;
                        continue;
                    }
                } else if (in_string) {
                    color = C_SYN_STRING;
                    if (c == '"' && gb_char_at(&ed.text, pos - 1) != '\\') {
                        in_string = 0;
                    }
                } else if (in_char) {
                    color = C_SYN_STRING;
                    if (c == '\'' && gb_char_at(&ed.text, pos - 1) != '\\') {
                        in_char = 0;
                    }
                } else {
                    if (c == '/' && pos + 1 < line_end) {
                        char n1 = gb_char_at(&ed.text, pos + 1);
                        if (n1 == '/') {
                            in_line_comment = 1;
                            color = C_SYN_COMMENT;
                        } else if (n1 == '*') {
                            in_block_comment = 1;
                            color = C_SYN_COMMENT;
                        }
                    } else if (c == '"') {
                        in_string = 1;
                        color = C_SYN_STRING;
                    } else if (c == '\'') {
                        in_char = 1;
                        color = C_SYN_STRING;
                    } else if (c == '#') {
                        int len = word_len_at(pos + 1, line_end);
                        if (len > 0 && check_kw_gb(pos + 1, len, c_kwd_pp)) {
                            color = C_SYN_DIRECTIVE;
                            token_len = len + 1;
                            render_token = 1;
                        }
                    } else if (is_digit(c) && is_word_start_at(pos, line_start)) {
                        token_len = word_len_at(pos, line_end);
                        color = C_SYN_NUMBER;
                        render_token = (token_len > 0);
                    } else if (is_word_start_at(pos, line_start)) {
                        token_len = word_len_at(pos, line_end);
                        if (token_len > 0) {
                            if (check_kw_gb(pos, token_len, c_kwd_types)) {
                                color = C_SYN_KEYWORD;
                            } else if (check_kw_gb(pos, token_len, c_kwd_ctrl)) {
                                color = C_SYN_CONTROL;
                            }
                            render_token = 1;
                        }
                    }
                }
            } else if (ed.lang == LANG_ASM) {
                if (in_string) {
                    color = C_SYN_STRING;
                    if (c == '"' && gb_char_at(&ed.text, pos - 1) != '\\') {
                        in_string = 0;
                    }
                } else {
                    if (c == ';') {
                        in_line_comment = 1;
                        color = C_SYN_COMMENT;
                    } else if (c == '"') {
                        in_string = 1;
                        color = C_SYN_STRING;
                    } else if (is_digit(c) && is_word_start_at(pos, line_start)) {
                        token_len = word_len_at(pos, line_end);
                        color = C_SYN_NUMBER;
                        render_token = (token_len > 0);
                    } else if (is_word_start_at(pos, line_start)) {
                        token_len = word_len_at(pos, line_end);
                        if (token_len > 0) {
                            if (check_kw_gb(pos, token_len, kwd_general)) {
                                color = C_SYN_KEYWORD;
                            } else if (check_kw_gb(pos, token_len, kwd_control)) {
                                color = C_SYN_CONTROL;
                            } else if (check_kw_gb(pos, token_len, kwd_dirs)) {
                                color = C_SYN_DIRECTIVE;
                            } else if (check_kw_gb(pos, token_len, kwd_regs)) {
                                color = C_SYN_REG;
                            }
                            render_token = 1;
                        }
                    }
                }
            }

            if (render_token && token_len > 0) {
                render_span(line_y, &col, pos, token_len, color, has_sel, sel_start, sel_end);
                pos += token_len;
                continue;
            }

            if (has_sel && pos >= sel_start && pos < sel_end) {
                draw_rect(GUTTER_W + PAD_X + col * CHAR_W, line_y, CHAR_W, LINE_H, C_SELECTION);
            }

            if (c == '\t') {
                int spaces = 4 - (col % 4);
                for (int i = 0; i < spaces; i++) {
                    render_char(canvas, WIN_W, WIN_H, GUTTER_W + PAD_X + (col + i) * CHAR_W, line_y, ' ', color);
                }
                col += spaces;
            } else {
                render_char(canvas, WIN_W, WIN_H, GUTTER_W + PAD_X + col * CHAR_W, line_y, c, color);
                col++;
            }

            if (in_line_comment) {
                for (int i = pos + 1; i < line_end; i++) {
                    char cc = gb_char_at(&ed.text, i);
                    render_char(canvas, WIN_W, WIN_H, GUTTER_W + PAD_X + col * CHAR_W, line_y, cc, C_SYN_COMMENT);
                    col++;
                }
                break;
            }

            pos++;
        }

        line_y += LINE_H;
    }

    if (ed.cursor >= 0 && ed.cursor <= text_len) {
        int line = lines_find_line(&ed.lines, ed.cursor);
        if (line < 0) {
            line = 0;
        }
        if (line >= ed.lines.count) {
            line = ed.lines.count - 1;
        }
        if (line < 0) {
            line = 0;
        }
        int line_start = ed.lines.starts[line];
        int col = 0;
        for (int i = line_start; i < ed.cursor && i < text_len; i++) {
            char c = gb_char_at(&ed.text, i);
            if (c == '\t') {
                col += 4 - (col % 4);
            } else {
                col++;
            }
        }
        int cursor_y = TAB_H + (line - ed.scroll_y) * LINE_H;
        int cursor_x = GUTTER_W + PAD_X + col * CHAR_W;

        if (cursor_y >= TAB_H && cursor_y + LINE_H <= WIN_H - STATUS_H) {
            draw_rect(cursor_x, cursor_y, 2, LINE_H, C_CURSOR);
        }
    }
}
