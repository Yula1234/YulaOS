// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#include "paint_input.h"

#include "paint_canvas.h"
#include "paint_image.h"
#include "paint_ui.h"

void handle_mouse_down(int mx, int my) {
    g_dbg_stage = 311;
    int ix = 0;
    int iy = 0;
    if (pt_in_rect(mx, my, r_toolbar)) {
        g_dbg_stage = 312;
        int p = palette_hit(mx, my);
        if (p >= 0) {
            cur_color = palette[p];
            return;
        }
        int ry = my - r_toolbar.y;
        if (ry >= 10 && ry < 34) {
            tool = TOOL_BRUSH;
        } else if (ry >= 36 && ry < 60) {
            tool = TOOL_ERASER;
        } else if (ry >= 62 && ry < 86) {
            tool = TOOL_LINE;
        } else if (ry >= 88 && ry < 112) {
            tool = TOOL_RECT;
        } else if (ry >= 114 && ry < 138) {
            tool = TOOL_CIRCLE;
        } else if (ry >= 140 && ry < 164) {
            tool = TOOL_FILL;
        } else if (ry >= 166 && ry < 190) {
            tool = TOOL_PICK;
        }
        mouse_down = 0;
        drag_active = 0;
        return;
    }
    g_dbg_stage = 313;
    if (!mouse_to_img(mx, my, &ix, &iy)) {
        return;
    }
    const size_t img_count = img_pixel_count();
    if (img_count == 0) {
        return;
    }
    const size_t idx = (size_t)(uint32_t)iy * (size_t)(uint32_t)img_w + (size_t)(uint32_t)ix;
    if (idx >= img_count) {
        return;
    }
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

void handle_mouse_move(int mx, int my) {
    if (!mouse_down) {
        return;
    }
    int ix = 0;
    int iy = 0;
    if (!mouse_to_img(mx, my, &ix, &iy)) {
        return;
    }
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

void handle_mouse_up(void) {
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

int handle_key(unsigned char c) {
    if (c == 'b' || c == 'B') {
        tool = TOOL_BRUSH;
    } else if (c == 'e' || c == 'E') {
        tool = TOOL_ERASER;
    } else if (c == 'l' || c == 'L') {
        tool = TOOL_LINE;
    } else if (c == 'r' || c == 'R') {
        tool = TOOL_RECT;
    } else if (c == 'c' || c == 'C') {
        tool = TOOL_CIRCLE;
    } else if (c == 'f' || c == 'F') {
        tool = TOOL_FILL;
    } else if (c == 'p' || c == 'P') {
        tool = TOOL_PICK;
    } else if (c == '+' || c == '=') {
        if (brush_r < 32) {
            brush_r++;
        }
    } else if (c == '-' || c == '_') {
        if (brush_r > 0) {
            brush_r--;
        }
    } else if (c == 'g' || c == 'G') {
        shape_fill = !shape_fill;
    } else if (c == 0x1A) {
        do_undo();
    } else if (c == 0x19) {
        do_redo();
    } else if (c >= '1' && c <= '8') {
        cur_color = palette[(int)(c - '1')];
    }
    return 0;
}
