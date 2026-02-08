// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#include "paint_state.h"

int WIN_W = 800;
int WIN_H = 600;

volatile int g_dbg_stage;
volatile int32_t g_dbg_resize_w;
volatile int32_t g_dbg_resize_h;

uint32_t* canvas;

rect_t r_header;
rect_t r_toolbar;
rect_t r_status;
rect_t r_canvas;

uint32_t* img;
int img_w;
int img_h;
int img_shm_fd = -1;
char img_shm_name[32];
uint32_t img_cap_bytes;
int img_shm_gen;

const uint32_t palette[8] = {
    0x000000,
    0xFFFFFF,
    0xFF0000,
    0x00FF00,
    0x0000FF,
    0xFFFF00,
    0xFF00FF,
    0x00FFFF,
};

int tool = TOOL_BRUSH;
int brush_r = 2;
uint32_t cur_color = 0x111111;
int shape_fill = 0;

int mouse_down;
int drag_active;
int drag_start_x;
int drag_start_y;
int drag_cur_x;
int drag_cur_y;
int last_img_x;
int last_img_y;

snapshot_t undo_stack[1];
int undo_count;

snapshot_t redo_stack[1];
int redo_count;
