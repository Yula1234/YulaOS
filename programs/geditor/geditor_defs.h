#pragma once

#include <yula.h>
#include <comp.h>
#include <font.h>

#define C_BG            0x1E1E1E
#define C_GUTTER_BG     0x181818
#define C_GUTTER_FG     0x7A7A7A
#define C_ACTIVE_LINE   0x262626
#define C_SELECTION     0x264F78
#define C_STATUS_BG     0x202020
#define C_STATUS_FG     0xD4D4D4
#define C_TAB_BG        0x252526
#define C_TAB_FG        0xD4D4D4
#define C_TEXT          0xD4D4D4
#define C_CURSOR        0xE6E6E6

#define C_UI_BORDER     0x333333
#define C_UI_ACCENT     0x3B8EEA
#define C_UI_MUTED      0x9A9A9A
#define C_UI_OK         0x3FB950
#define C_UI_ERROR      0xF85149
#define C_MINI_BG       0x1A1A1A
#define C_MINI_BORDER   0x3A3A3A

#define C_SYN_KEYWORD   0x569CD6
#define C_SYN_CONTROL   0xC586C0
#define C_SYN_DIRECTIVE 0x4EC9B0
#define C_SYN_NUMBER    0xB5CEA8
#define C_SYN_STRING    0xCE9178
#define C_SYN_COMMENT   0x6A9955
#define C_SYN_REG       0x9CDCFE

#define LINE_H      16
#define CHAR_W      8
#define GUTTER_W    48
#define STATUS_H    24
#define TAB_H       24
#define PAD_X       8

typedef struct {
    char* buf;
    int cap;
    int gap_start;
    int gap_end;
} GapBuf;

typedef struct {
    int* starts;
    int count;
    int cap;
    uint8_t* c_block;
    int c_block_cap;
} LineIndex;

typedef struct {
    int type;
    int pos;
    int len;
    char* text;
} UndoAction;

typedef struct {
    UndoAction* items;
    int count;
    int cap;
} UndoStack;

enum {
    LANG_ASM = 0,
    LANG_C = 1,
};

enum {
    MODE_EDIT = 0,
    MODE_FIND = 1,
    MODE_GOTO = 2,
    MODE_OPEN = 3,
};

enum {
    UNDO_INSERT = 1,
    UNDO_DELETE = 2,
};

typedef struct {
    GapBuf text;
    LineIndex lines;

    int cursor;
    int sel_bound;
    int scroll_y;
    char filename[256];
    int dirty;
    int quit;
    int pref_col;
    int is_dragging;

    int lang;
    int mode;
    char mini[256];
    int mini_len;

    int open_confirm;

    char find[64];
    int find_len;

    char status[64];
    int status_len;
    uint32_t status_color;

    UndoStack undo;
    UndoStack redo;
} Editor;

extern int WIN_W;
extern int WIN_H;
extern uint32_t* canvas;
extern Editor ed;

extern const uint32_t surface_id;

extern comp_conn_t conn;
extern char shm_name[32];
extern int shm_fd;
extern int shm_gen;
extern uint32_t size_bytes;

extern const char* kwd_general[];
extern const char* kwd_control[];
extern const char* kwd_dirs[];
extern const char* kwd_regs[];
extern const char* c_kwd_types[];
extern const char* c_kwd_ctrl[];
extern const char* c_kwd_pp[];
