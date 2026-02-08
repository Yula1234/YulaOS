// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#pragma once

#include "paint_state.h"

void layout_update(void);
void render_all(void);
int mouse_to_img(int mx, int my, int* out_x, int* out_y);
void ui_draw_tool_item(int y, const char* label, int is_active);
int tool_name(char* out, int out_cap);
rect_t palette_rect(int idx);
int palette_hit(int mx, int my);
