#include "ui.h"
#include "editor.h"
#include "lines.h"
#include "render.h"
#include "util.h"

void render_ui(void) {
    draw_rect(0, 0, WIN_W, TAB_H, C_TAB_BG);
    draw_rect(0, WIN_H - STATUS_H, WIN_W, STATUS_H, C_STATUS_BG);

    const char* base = path_base(ed.filename);
    if (!base || !base[0]) base = "Untitled";

    char title[64];
    int max_chars = (WIN_W - 200) / CHAR_W;
    if (max_chars < 4) max_chars = 4;
    if (max_chars > (int)sizeof(title) - 1) max_chars = (int)sizeof(title) - 1;
    fmt_title_ellipsis(base, title, (int)sizeof(title), max_chars);

    char display[80];
    int tn = (int)strlen(title);
    if (tn > (int)sizeof(display) - 4) tn = (int)sizeof(display) - 4;
    for (int i = 0; i < tn; i++) display[i] = title[i];
    if (ed.dirty) {
        display[tn++] = ' ';
        display[tn++] = '*';
    }
    display[tn] = 0;

    int ty = 4;
    render_string(8, ty, display, C_TAB_FG);

    char mode[16] = "";
    if (ed.lang == LANG_C) {
        mode[0] = 'C'; mode[1] = 0;
    } else if (ed.lang == LANG_ASM) {
        mode[0] = 'A'; mode[1] = 'S'; mode[2] = 'M'; mode[3] = 0;
    }
    int mode_x = WIN_W - 40;
    render_string(mode_x, ty, mode, C_UI_MUTED);

    int status_text_y = WIN_H - STATUS_H + 4;

    int line = 1;
    int col = 1;
    if (ed.lines.count <= 0) lines_rebuild(&ed.lines, &ed.text, ed.lang);
    if (ed.lines.count > 0) {
        int li = lines_find_line(&ed.lines, ed.cursor);
        if (li < 0) li = 0;
        if (li >= ed.lines.count) li = ed.lines.count - 1;
        line = li + 1;
        col = ed.cursor - ed.lines.starts[li] + 1;
    }

    char buf_line[16];
    char buf_col[16];
    fmt_int(line, buf_line);
    fmt_int(col, buf_col);

    int right_w = 210;
    int right_x = WIN_W - right_w;
    draw_rect(right_x, WIN_H - STATUS_H, right_w, STATUS_H, C_STATUS_BG);
    render_string(right_x + 8, status_text_y, "Ln", C_UI_MUTED);
    render_string(right_x + 8 + 3 * CHAR_W, status_text_y, buf_line, C_STATUS_FG);
    render_string(right_x + 64, status_text_y, "Col", C_UI_MUTED);
    render_string(right_x + 64 + 4 * CHAR_W, status_text_y, buf_col, C_STATUS_FG);

    if (ed.mode == MODE_FIND) {
        int right_w = 210;
        int px = 10;
        render_string(px, status_text_y, "Find:", C_UI_MUTED);
        int bx = px + 6 * CHAR_W + 8;
        int bw = (WIN_W - right_w) - bx - 10;
        if (bw < 80) bw = 80;
        int by = status_text_y + 1;
        int bh = 16 + 2;
        draw_rect(bx, by, bw, bh, C_MINI_BG);
        draw_rect(bx, by, bw, 1, C_MINI_BORDER);
        draw_rect(bx, by + bh - 1, bw, 1, C_MINI_BORDER);
        draw_rect(bx, by, 1, bh, C_MINI_BORDER);
        draw_rect(bx + bw - 1, by, 1, bh, C_MINI_BORDER);

        int ix = bx + 6;
        render_string(ix, status_text_y, ed.mini, C_STATUS_FG);
        int cx = ix + ed.mini_len * CHAR_W;
        if (cx > bx + bw - 4) cx = bx + bw - 4;
        draw_rect(cx, status_text_y + 1, 2, 16, C_CURSOR);
    }
    else if (ed.mode == MODE_GOTO) {
        int right_w = 210;
        int px = 10;
        render_string(px, status_text_y, "Goto:", C_UI_MUTED);
        int bx = px + 6 * CHAR_W + 8;
        int bw = (WIN_W - right_w) - bx - 10;
        if (bw < 80) bw = 80;

        int text_y = status_text_y;
        int glyph_top = text_y + 1;
        int pad_y = 1;
        int box_shift = 0;
        int by = glyph_top - pad_y + box_shift;
        int bh = 16 + pad_y * 2;
        draw_rect(bx, by, bw, bh, C_MINI_BG);
        draw_rect(bx, by, bw, 1, C_MINI_BORDER);
        draw_rect(bx, by + bh - 1, bw, 1, C_MINI_BORDER);
        draw_rect(bx, by, 1, bh, C_MINI_BORDER);
        draw_rect(bx + bw - 1, by, 1, bh, C_MINI_BORDER);

        int ix = bx + 6;
        render_string(ix, text_y, ed.mini, C_STATUS_FG);
        int cx = ix + ed.mini_len * CHAR_W;
        if (cx > bx + bw - 4) cx = bx + bw - 4;
        int cy = glyph_top;
        int ch = 16;
        draw_rect(cx, cy, 2, ch, C_CURSOR);
    }
    else if (ed.mode == MODE_OPEN) {
        int right_w = 210;
        int px = 10;
        render_string(px, status_text_y, "Open:", C_UI_MUTED);
        int bx = px + 6 * CHAR_W + 8;
        int bw = (WIN_W - right_w) - bx - 10;
        if (bw < 80) bw = 80;

        int max_chars = (bw - 12) / CHAR_W;
        if (max_chars < 4) max_chars = 4;
        if (max_chars > (int)sizeof(ed.mini) - 1) max_chars = (int)sizeof(ed.mini) - 1;
        char disp[256];
        fmt_title_ellipsis(ed.mini, disp, (int)sizeof(disp), max_chars);

        int text_y = status_text_y;
        int glyph_top = text_y + 1;
        int pad_y = 1;
        int box_shift = 0;
        int by = glyph_top - pad_y + box_shift;
        int bh = 16 + pad_y * 2;
        draw_rect(bx, by, bw, bh, C_MINI_BG);
        draw_rect(bx, by, bw, 1, C_MINI_BORDER);
        draw_rect(bx, by + bh - 1, bw, 1, C_MINI_BORDER);
        draw_rect(bx, by, 1, bh, C_MINI_BORDER);
        draw_rect(bx + bw - 1, by, 1, bh, C_MINI_BORDER);

        int ix = bx + 6;
        render_string(ix, text_y, disp, C_STATUS_FG);
        int disp_len = (int)strlen(disp);
        int cx = ix + disp_len * CHAR_W;
        if (cx > bx + bw - 4) cx = bx + bw - 4;
        int cy = glyph_top;
        int ch = 16;
        draw_rect(cx, cy, 2, ch, C_CURSOR);
    }
    else if (ed.status_len > 0) {
        render_string(10, status_text_y, ed.status, ed.status_color ? ed.status_color : C_UI_MUTED);
    }
}
