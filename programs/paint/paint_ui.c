// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#include "paint_ui.h"

#include "paint_canvas.h"
#include "paint_image.h"
#include "paint_util.h"

void layout_update(void) {
    r_header.x = 0;
    r_header.y = 0;
    r_header.w = WIN_W;
    r_header.h = UI_TOP_H;
    r_status.x = 0;
    r_status.y = WIN_H - UI_STATUS_H;
    r_status.w = WIN_W;
    r_status.h = UI_STATUS_H;
    int middle_h = WIN_H - UI_TOP_H - UI_STATUS_H;
    if (middle_h < 0) {
        middle_h = 0;
    }
    r_toolbar.x = 0;
    r_toolbar.y = UI_TOP_H;
    r_toolbar.w = UI_TOOL_W;
    r_toolbar.h = middle_h;
    r_canvas.x = UI_TOOL_W;
    r_canvas.y = UI_TOP_H;
    r_canvas.w = WIN_W - UI_TOOL_W;
    r_canvas.h = middle_h;
    if (r_canvas.w < 0) {
        r_canvas.w = 0;
    }
    if (r_canvas.h < 0) {
        r_canvas.h = 0;
    }
}

int tool_name(char* out, int out_cap) {
    const char* s = "Brush";
    if (tool == TOOL_ERASER) {
        s = "Eraser";
    } else if (tool == TOOL_LINE) {
        s = "Line";
    } else if (tool == TOOL_RECT) {
        s = "Rect";
    } else if (tool == TOOL_CIRCLE) {
        s = "Circle";
    } else if (tool == TOOL_FILL) {
        s = "Fill";
    } else if (tool == TOOL_PICK) {
        s = "Pick";
    }
    return snprintf(out, (size_t)out_cap, "%s", s);
}

void ui_draw_tool_item(int y, const char* label, int is_active) {
    int x = 8;
    int w = r_toolbar.w - 16;
    int h = 24;
    if (is_active) {
        fill_rect(r_toolbar.x + x, r_toolbar.y + y, w, h, 0x1B1B1C);
        draw_frame(r_toolbar.x + x, r_toolbar.y + y, w, h, C_ACCENT);
    } else {
        fill_rect(r_toolbar.x + x, r_toolbar.y + y, w, h, 0x1E1E1E);
        draw_frame(r_toolbar.x + x, r_toolbar.y + y, w, h, C_BORDER);
    }
    draw_string(canvas, WIN_W, WIN_H, r_toolbar.x + x + 6, r_toolbar.y + y + 4, label, is_active ? C_TEXT : C_TEXT_DIM);
}

rect_t palette_rect(int idx) {
    int cols = 3;
    int sw = 18;
    int gap = 6;
    int color_bar_y = r_toolbar.y + r_toolbar.h - 60;
    int rows = (8 + cols - 1) / cols;
    int pal_h = rows * sw + (rows - 1) * gap;
    int label_h = 22;
    int pad = 2;
    int py = color_bar_y - (pal_h + label_h + pad);
    int min_py = r_toolbar.y + 236;
    int max_py = color_bar_y - pal_h - pad;
    if (max_py < min_py) {
        py = min_py;
    } else {
        if (py < min_py) {
            py = min_py;
        }
        if (py > max_py) {
            py = max_py;
        }
    }
    int row = idx / cols;
    int col = idx % cols;
    int x = r_toolbar.x + 10 + col * (sw + gap);
    int y = py + row * (sw + gap);
    rect_t r;
    r.x = x;
    r.y = y;
    r.w = sw;
    r.h = sw;
    return r;
}

int palette_hit(int mx, int my) {
    for (int i = 0; i < 8; i++) {
        rect_t pr = palette_rect(i);
        if (pt_in_rect(mx, my, pr)) {
            return i;
        }
    }
    return -1;
}

void render_all(void) {
    fill_rect(0, 0, WIN_W, WIN_H, C_WIN_BG);
    fill_rect(r_header.x, r_header.y, r_header.w, r_header.h, C_HEADER_BG);
    draw_frame(r_header.x, r_header.y, r_header.w, r_header.h, 0x000000);
    draw_string(canvas, WIN_W, WIN_H, 10, 14, "Paint", C_TEXT);
    fill_rect(r_toolbar.x, r_toolbar.y, r_toolbar.w, r_toolbar.h, C_PANEL_BG);
    draw_frame(r_toolbar.x, r_toolbar.y, r_toolbar.w, r_toolbar.h, C_BORDER);
    ui_draw_tool_item(10, "Brush (B)", tool == TOOL_BRUSH);
    ui_draw_tool_item(36, "Eraser (E)", tool == TOOL_ERASER);
    ui_draw_tool_item(62, "Line (L)", tool == TOOL_LINE);
    ui_draw_tool_item(88, "Rect (R)", tool == TOOL_RECT);
    ui_draw_tool_item(114, "Circle (C)", tool == TOOL_CIRCLE);
    ui_draw_tool_item(140, "Fill (F)", tool == TOOL_FILL);
    ui_draw_tool_item(166, "Pick (P)", tool == TOOL_PICK);
    int cy = 200;
    draw_string(canvas, WIN_W, WIN_H, r_toolbar.x + 10, r_toolbar.y + cy, "Size:", C_TEXT_DIM);
    char sbuf[32];
    snprintf(sbuf, sizeof(sbuf), "%d", brush_r);
    draw_string(canvas, WIN_W, WIN_H, r_toolbar.x + 54, r_toolbar.y + cy, sbuf, C_TEXT);
    draw_string(canvas, WIN_W, WIN_H, r_toolbar.x + 10, r_toolbar.y + cy + 22, "-/+", C_TEXT_DIM);
    fill_rect(r_status.x, r_status.y, r_status.w, r_status.h, C_HEADER_BG);
    draw_frame(r_status.x, r_status.y, r_status.w, r_status.h, 0x000000);
    char tbuf[64];
    tool_name(tbuf, sizeof(tbuf));
    char st[96];
    snprintf(st, sizeof(st), "Tool: %s  Undo:%d  Redo:%d", tbuf, undo_count, redo_count);
    draw_string(canvas, WIN_W, WIN_H, 8, r_status.y + 6, st, C_TEXT_DIM);
    fill_rect(r_canvas.x, r_canvas.y, r_canvas.w, r_canvas.h, C_CANVAS_BG);
    draw_frame(r_canvas.x, r_canvas.y, r_canvas.w, r_canvas.h, C_BORDER);
    if (img && !ptr_is_invalid(img) && img_w > 0 && img_h > 0) {
        size_t count = img_pixel_count();
        if (count != 0) {
            int cw = min_i(img_w, r_canvas.w);
            int ch = min_i(img_h, r_canvas.h);
            for (int y = 0; y < ch; y++) {
                memcpy(
                    canvas + (r_canvas.y + y) * WIN_W + r_canvas.x,
                    img + y * img_w,
                    (size_t)cw * 4u
                );
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
        draw_string(canvas, WIN_W, WIN_H, r_header.w - 90, 14, "FILL", C_ACCENT);
    }
    rect_t p0 = palette_rect(0);
    draw_string(canvas, WIN_W, WIN_H, r_toolbar.x + 10, p0.y - 20, "Colors:", C_TEXT_DIM);
    for (int i = 0; i < 8; i++) {
        rect_t pr = palette_rect(i);
        fill_rect(pr.x, pr.y, pr.w, pr.h, palette[i]);
        draw_frame(pr.x, pr.y, pr.w, pr.h, (palette[i] == cur_color) ? C_ACCENT : 0x000000);
    }
    fill_rect(r_toolbar.x + 10, r_toolbar.y + r_toolbar.h - 60, r_toolbar.w - 20, 20, cur_color);
    draw_frame(r_toolbar.x + 10, r_toolbar.y + r_toolbar.h - 60, r_toolbar.w - 20, 20, 0x000000);
    draw_string(canvas, WIN_W, WIN_H, r_toolbar.x + 10, r_toolbar.y + r_toolbar.h - 40, "Ctrl+Z/Y", C_TEXT_DIM);
}

int mouse_to_img(int mx, int my, int* out_x, int* out_y) {
    size_t count = img_pixel_count();
    if (count == 0) {
        return 0;
    }
    if (!pt_in_rect(mx, my, r_canvas)) {
        return 0;
    }
    int ix = mx - r_canvas.x;
    int iy = my - r_canvas.y;
    if ((unsigned)ix >= (unsigned)img_w) {
        return 0;
    }
    if ((unsigned)iy >= (unsigned)img_h) {
        return 0;
    }
    *out_x = ix;
    *out_y = iy;
    return 1;
}
