// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#pragma once

#include <yula.h>
#include <comp.h>
#include <font.h>

#define PAINT_MAX_SURFACE_BYTES (32u * 1024u * 1024u)
#define PAINT_MAX_IMG_BYTES     (16u * 1024u * 1024u)

#define PAINT_MAX_SURFACE_PIXELS (PAINT_MAX_SURFACE_BYTES / 4u)
#define PAINT_MAX_IMG_PIXELS     (PAINT_MAX_IMG_BYTES / 4u)

#define C_WIN_BG    0x1E1E1E
#define C_PANEL_BG  0x252526
#define C_HEADER_BG 0x2D2D2D
#define C_BORDER    0x3E3E42
#define C_TEXT      0xD4D4D4
#define C_TEXT_DIM  0x9A9A9A
#define C_ACCENT    0x007ACC
#define C_CANVAS_BG 0xFFFFFF

#define UI_TOP_H    44
#define UI_STATUS_H 28
#define UI_TOOL_W   96

typedef struct {
    int x;
    int y;
    int w;
    int h;
} rect_t;

enum {
    TOOL_BRUSH = 0,
    TOOL_ERASER = 1,
    TOOL_LINE = 2,
    TOOL_RECT = 3,
    TOOL_CIRCLE = 4,
    TOOL_FILL = 5,
    TOOL_PICK = 6,
};

typedef struct {
    uint32_t* pixels;
    int w;
    int h;
    int shm_fd;
    uint32_t cap_bytes;
    char shm_name[32];
    uint32_t shm_gen;
    char tag;
} snapshot_t;

extern int WIN_W;
extern int WIN_H;

extern volatile int g_dbg_stage;
extern volatile int32_t g_dbg_resize_w;
extern volatile int32_t g_dbg_resize_h;

extern uint32_t* canvas;

extern rect_t r_header;
extern rect_t r_toolbar;
extern rect_t r_status;
extern rect_t r_canvas;

extern uint32_t* img;
extern int img_w;
extern int img_h;
extern int img_shm_fd;
extern char img_shm_name[32];
extern uint32_t img_cap_bytes;
extern int img_shm_gen;

extern const uint32_t palette[8];

extern int tool;
extern int brush_r;
extern uint32_t cur_color;
extern int shape_fill;

extern int mouse_down;
extern int drag_active;
extern int drag_start_x;
extern int drag_start_y;
extern int drag_cur_x;
extern int drag_cur_y;
extern int last_img_x;
extern int last_img_y;

extern snapshot_t undo_stack[1];
extern int undo_count;

extern snapshot_t redo_stack[1];
extern int redo_count;
