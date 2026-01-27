// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include <yula.h>
#include <comp.h>
#include <font.h>

static int WIN_W = 800;
static int WIN_H = 600;

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

#define C_SYN_KEYWORD   0x569CD6 // blue
#define C_SYN_CONTROL   0xC586C0 // purple
#define C_SYN_DIRECTIVE 0x4EC9B0 // teal
#define C_SYN_NUMBER    0xB5CEA8 // light green
#define C_SYN_STRING    0xCE9178 // orange
#define C_SYN_COMMENT   0x6A9955 // green
#define C_SYN_REG       0x9CDCFE // light blue

#define LINE_H      18
#define CHAR_W      8
#define GUTTER_W    48
#define STATUS_H    24
#define TAB_H       32
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

uint32_t* canvas;
Editor ed;

static const uint32_t surface_id = 1u;

static comp_conn_t conn;
static char shm_name[32];
static int shm_fd = -1;
static int shm_gen = 0;
static uint32_t size_bytes = 0;

const char* kwd_general[] = {
    "mov", "lea", "push", "pop", "add", "sub", "imul", "div", "xor", "or", "and", 
    "cmp", "test", "inc", "dec", "hlt", "cli", "sti", "nop", "int", "shl", "shr", 
    "rol", "ror", "neg", "not", 0
};

const char* kwd_control[] = {
    "jmp", "je", "jne", "jg", "jge", "jl", "jle", "jz", "jnz", "call", "ret", "loop", 
    "ja", "jb", "jae", "jbe", 0
};

const char* kwd_dirs[] = {
    "section", "global", "extern", "public", "db", "dw", "dd", "dq", "rb", "resb", 
    "use32", "format", "org", "entry", "byte", "word", "dword", "ptr", "equ", 0
};

const char* kwd_regs[] = {
    "eax", "ebx", "ecx", "edx", "esi", "edi", "esp", "ebp", "ax", "bx", "cx", "dx", 
    "al", "ah", "bl", "bh", "dl", "dh", "cl", "ch", 0
};

const char* c_kwd_types[] = {
    "void", "char", "short", "int", "long", "signed", "unsigned", "float", "double",
    "struct", "union", "enum", "typedef", "const", "volatile", "static", "extern",
    "register", "auto", "inline", "sizeof", 0
};

const char* c_kwd_ctrl[] = {
    "if", "else", "for", "while", "do", "switch", "case", "default", "break",
    "continue", "return", "goto", 0
};

const char* c_kwd_pp[] = {
    "include", "define", "undef", "ifdef", "ifndef", "if", "elif", "else",
    "endif", "error", "pragma", 0
};

int min(int a, int b) { return a < b ? a : b; }
int max(int a, int b) { return a > b ? a : b; }

int is_digit(char c) { return (c >= '0' && c <= '9'); }
int is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '.'; }
int is_word_char(char c) { return is_alpha(c) || is_digit(c); }

static char lower_char(char c) {
    if (c >= 'A' && c <= 'Z') return (char)(c + ('a' - 'A'));
    return c;
}

static const char* path_ext(const char* s) {
    if (!s) return 0;
    int n = strlen(s);
    for (int i = n - 1; i >= 0; i--) {
        char c = s[i];
        if (c == '.') return s + i + 1;
        if (c == '/' || c == '\\') break;
    }
    return 0;
}

static const char* path_base(const char* s) {
    if (!s) return "";
    const char* last = s;
    for (const char* p = s; *p; p++) {
        if (*p == '/' || *p == '\\') last = p + 1;
    }
    return last;
}

static void fmt_title_ellipsis(const char* s, char* out, int out_cap, int max_chars) {
    if (!out || out_cap <= 0) return;
    out[0] = 0;
    if (!s) return;
    int n = (int)strlen(s);
    if (max_chars < 4) max_chars = 4;
    if (max_chars > out_cap - 1) max_chars = out_cap - 1;
    if (n <= max_chars) {
        for (int i = 0; i < n; i++) out[i] = s[i];
        out[n] = 0;
        return;
    }
    out[0] = '.'; out[1] = '.'; out[2] = '.';
    int keep = max_chars - 3;
    for (int i = 0; i < keep; i++) out[3 + i] = s[n - keep + i];
    out[max_chars] = 0;
}

static int gb_len(const GapBuf* g);
static char gb_char_at(const GapBuf* g, int pos);

static int load_file(void);
static int load_file_silent(void);
static int load_file_impl(int show_status);

static int lines_ensure(LineIndex* li, int need);
static void lines_rebuild(LineIndex* li, const GapBuf* g, int lang);
static int lines_find_line(const LineIndex* li, int pos);

static int lines_lower_bound(const LineIndex* li, int key) {
    if (!li || li->count <= 0) return 0;
    int lo = 0;
    int hi = li->count;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (li->starts[mid] < key) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

static int lines_upper_bound(const LineIndex* li, int key) {
    if (!li || li->count <= 0) return 0;
    int lo = 0;
    int hi = li->count;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (li->starts[mid] <= key) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

static void lines_recompute_c_block_from(LineIndex* li, const GapBuf* g, int from_line) {
    if (!li || !g) return;
    if (li->count <= 0) return;
    if (from_line < 0) from_line = 0;
    if (from_line >= li->count) return;
    if (!li->c_block) return;

    int in_block = li->c_block[from_line] ? 1 : 0;
    int text_len = gb_len(g);
    int line = from_line;

    for (int i = li->starts[from_line]; i < text_len && line + 1 < li->count; i++) {
        char c = gb_char_at(g, i);
        char n1 = (i + 1 < text_len) ? gb_char_at(g, i + 1) : 0;
        if (!in_block && c == '/' && n1 == '*') { in_block = 1; i++; }
        else if (in_block && c == '*' && n1 == '/') { in_block = 0; i++; }

        if (c == '\n') {
            line++;
            li->c_block[line] = (uint8_t)in_block;
        }
    }
}

static int lines_apply_insert(LineIndex* li, const GapBuf* g, int pos, const char* s, int slen, int lang) {
    if (!li || !g) return 0;
    if (li->count <= 0) {
        lines_rebuild(li, g, lang);
        return 1;
    }

    int line = lines_find_line(li, pos);
    if (line < 0) line = 0;
    if (line >= li->count) line = li->count - 1;

    int nl = 0;
    for (int i = 0; i < slen; i++) if (s[i] == '\n') nl++;

    if (nl > 0) {
        if (!lines_ensure(li, li->count + nl)) return 0;

        int insert_at = line + 1;
        int tail = li->count - insert_at;
        if (tail > 0) {
            memmove(&li->starts[insert_at + nl], &li->starts[insert_at], (size_t)tail * sizeof(int));
            memmove(&li->c_block[insert_at + nl], &li->c_block[insert_at], (size_t)tail);
        }

        int j = 0;
        for (int i = 0; i < slen; i++) {
            if (s[i] == '\n') {
                li->starts[insert_at + j] = pos + i + 1;
                li->c_block[insert_at + j] = 0;
                j++;
            }
        }
        li->count += nl;

        for (int i = insert_at + nl; i < li->count; i++) {
            li->starts[i] += slen;
        }
    } else {
        for (int i = line + 1; i < li->count; i++) {
            li->starts[i] += slen;
        }
    }

    if (lang == LANG_C) lines_recompute_c_block_from(li, g, line);
    return 1;
}

static int lines_apply_delete(LineIndex* li, const GapBuf* g, int start, int end, int lang) {
    if (!li || !g) return 0;
    if (li->count <= 0) {
        lines_rebuild(li, g, lang);
        return 1;
    }

    int delta = end - start;
    if (delta <= 0) return 1;

    int line = lines_find_line(li, start);
    if (line < 0) line = 0;
    if (line >= li->count) line = li->count - 1;

    int rm0 = lines_lower_bound(li, start + 1);
    int rm1 = lines_upper_bound(li, end);
    if (rm0 < 1) rm0 = 1;
    if (rm1 < rm0) rm1 = rm0;
    if (rm1 > li->count) rm1 = li->count;

    int rm = rm1 - rm0;
    if (rm > 0) {
        int tail = li->count - rm1;
        if (tail > 0) {
            memmove(&li->starts[rm0], &li->starts[rm1], (size_t)tail * sizeof(int));
            memmove(&li->c_block[rm0], &li->c_block[rm1], (size_t)tail);
        }
        li->count -= rm;
        if (line >= li->count) line = li->count - 1;
    }

    for (int i = rm0; i < li->count; i++) {
        li->starts[i] -= delta;
        if (li->starts[i] < 0) li->starts[i] = 0;
    }

    if (lang == LANG_C) lines_recompute_c_block_from(li, g, line);
    return 1;
}

static int ext_eq_ci(const char* ext, const char* lit) {
    if (!ext || !lit) return 0;
    int i = 0;
    while (ext[i] && lit[i]) {
        if (lower_char(ext[i]) != lower_char(lit[i])) return 0;
        i++;
    }
    return ext[i] == 0 && lit[i] == 0;
}

static int gb_len(const GapBuf* g) {
    return g->cap - (g->gap_end - g->gap_start);
}

static int gb_gap_size(const GapBuf* g) {
    return g->gap_end - g->gap_start;
}

static void gb_init(GapBuf* g, int initial_cap) {
    if (initial_cap < 64) initial_cap = 64;
    g->buf = (char*)malloc((size_t)initial_cap);
    g->cap = g->buf ? initial_cap : 0;
    g->gap_start = 0;
    g->gap_end = g->cap;
}

static void gb_destroy(GapBuf* g) {
    if (g->buf) free(g->buf);
    g->buf = 0;
    g->cap = 0;
    g->gap_start = 0;
    g->gap_end = 0;
}

static char gb_char_at(const GapBuf* g, int pos) {
    int len = gb_len(g);
    if (pos < 0 || pos >= len) return 0;
    if (pos < g->gap_start) return g->buf[pos];
    return g->buf[pos + gb_gap_size(g)];
}

static void gb_move_gap(GapBuf* g, int pos) {
    int len = gb_len(g);
    if (pos < 0) pos = 0;
    if (pos > len) pos = len;

    if (pos < g->gap_start) {
        int move = g->gap_start - pos;
        memmove(g->buf + g->gap_end - move, g->buf + pos, (size_t)move);
        g->gap_start -= move;
        g->gap_end -= move;
    } else if (pos > g->gap_start) {
        int move = pos - g->gap_start;
        memmove(g->buf + g->gap_start, g->buf + g->gap_end, (size_t)move);
        g->gap_start += move;
        g->gap_end += move;
    }
}

static int gb_ensure_gap(GapBuf* g, int need) {
    if (need <= gb_gap_size(g)) return 1;

    int len = gb_len(g);
    int new_cap = g->cap;
    while (new_cap - len < need) {
        if (new_cap < 1024) new_cap *= 2;
        else new_cap += new_cap / 2;
        if (new_cap < g->cap) return 0;
    }

    char* nb = (char*)malloc((size_t)new_cap);
    if (!nb) return 0;

    int before = g->gap_start;
    int after = g->cap - g->gap_end;
    if (before) memcpy(nb, g->buf, (size_t)before);
    if (after) memcpy(nb + (new_cap - after), g->buf + g->gap_end, (size_t)after);
    free(g->buf);
    g->buf = nb;
    g->cap = new_cap;
    g->gap_start = before;
    g->gap_end = new_cap - after;
    return 1;
}

static int gb_insert_at(GapBuf* g, int pos, const char* s, int slen) {
    if (slen <= 0) return 1;
    gb_move_gap(g, pos);
    if (!gb_ensure_gap(g, slen)) return 0;
    memcpy(g->buf + g->gap_start, s, (size_t)slen);
    g->gap_start += slen;
    return 1;
}

static int gb_delete_range(GapBuf* g, int start, int end) {
    int len = gb_len(g);
    if (start < 0) start = 0;
    if (end > len) end = len;
    if (start >= end) return 1;
    gb_move_gap(g, start);
    g->gap_end += (end - start);
    if (g->gap_end > g->cap) g->gap_end = g->cap;
    return 1;
}

static char* gb_copy_range(const GapBuf* g, int start, int end) {
    int len = gb_len(g);
    if (start < 0) start = 0;
    if (end > len) end = len;
    if (start >= end) {
        char* z = (char*)malloc(1);
        if (z) z[0] = 0;
        return z;
    }
    int n = end - start;
    char* out = (char*)malloc((size_t)n + 1);
    if (!out) return 0;
    for (int i = 0; i < n; i++) out[i] = gb_char_at(g, start + i);
    out[n] = 0;
    return out;
}

static void lines_init(LineIndex* li) {
    li->starts = 0;
    li->count = 0;
    li->cap = 0;
    li->c_block = 0;
    li->c_block_cap = 0;
}

static void lines_destroy(LineIndex* li) {
    if (li->starts) free(li->starts);
    if (li->c_block) free(li->c_block);
    li->starts = 0;
    li->c_block = 0;
    li->count = 0;
    li->cap = 0;
    li->c_block_cap = 0;
}

static int lines_ensure(LineIndex* li, int need) {
    if (need <= li->cap && need <= li->c_block_cap) return 1;
    int new_cap = li->cap ? li->cap : 64;
    while (new_cap < need) new_cap *= 2;
    int* ns = (int*)realloc(li->starts, (size_t)new_cap * sizeof(int));
    if (!ns) return 0;
    uint8_t* nb = (uint8_t*)realloc(li->c_block, (size_t)new_cap);
    if (!nb) {
        li->starts = ns;
        return 0;
    }
    li->starts = ns;
    li->c_block = nb;
    li->cap = new_cap;
    li->c_block_cap = new_cap;
    return 1;
}

static void lines_rebuild(LineIndex* li, const GapBuf* g, int lang) {
    int len = gb_len(g);
    int approx = 2;
    for (int i = 0; i < len; i++) if (gb_char_at(g, i) == '\n') approx++;
    if (!lines_ensure(li, approx)) {
        li->count = 0;
        return;
    }

    int n = 0;
    li->starts[n] = 0;
    li->c_block[n] = 0;
    n++;

    int in_block = 0;
    for (int i = 0; i < len; i++) {
        char c = gb_char_at(g, i);
        if (lang == LANG_C) {
            char n1 = (i + 1 < len) ? gb_char_at(g, i + 1) : 0;
            if (!in_block && c == '/' && n1 == '*') in_block = 1;
            else if (in_block && c == '*' && n1 == '/') in_block = 0;
        }
        if (c == '\n') {
            if (n >= li->cap) {
                if (!lines_ensure(li, li->cap * 2)) break;
            }
            li->starts[n] = i + 1;
            li->c_block[n] = (uint8_t)in_block;
            n++;
        }
    }
    li->count = n;
}

static int lines_find_line(const LineIndex* li, int pos) {
    if (!li || li->count <= 0) return 0;
    if (pos <= 0) return 0;
    int lo = 0;
    int hi = li->count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int s = li->starts[mid];
        if (s <= pos) {
            if (mid + 1 >= li->count || li->starts[mid + 1] > pos) return mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return 0;
}

static int get_line_start(int pos);
static int get_line_len(int start);
static void update_pref_col(void);
void delete_range(int start, int end);

static void uaction_free(UndoAction* a) {
    if (a->text) free(a->text);
    a->text = 0;
    a->len = 0;
    a->pos = 0;
    a->type = 0;
}

static void ustack_init(UndoStack* st) {
    st->items = 0;
    st->count = 0;
    st->cap = 0;
}

static void ustack_reset(UndoStack* st) {
    for (int i = 0; i < st->count; i++) uaction_free(&st->items[i]);
    st->count = 0;
}

static void ustack_destroy(UndoStack* st) {
    ustack_reset(st);
    if (st->items) free(st->items);
    st->items = 0;
    st->cap = 0;
}

static int ustack_push(UndoStack* st, UndoAction a) {
    if (st->count >= st->cap) {
        int new_cap = st->cap ? st->cap * 2 : 64;
        UndoAction* ni = (UndoAction*)realloc(st->items, (size_t)new_cap * sizeof(UndoAction));
        if (!ni) {
            uaction_free(&a);
            return 0;
        }
        st->items = ni;
        st->cap = new_cap;
    }
    st->items[st->count++] = a;
    return 1;
}

static UndoAction ustack_pop(UndoStack* st) {
    UndoAction a;
    a.type = 0;
    a.pos = 0;
    a.len = 0;
    a.text = 0;
    if (st->count <= 0) return a;
    a = st->items[--st->count];
    st->items[st->count].text = 0;
    return a;
}

static void editor_delete_raw(int start, int end) {
    gb_delete_range(&ed.text, start, end);
    if (!lines_apply_delete(&ed.lines, &ed.text, start, end, ed.lang)) {
        lines_rebuild(&ed.lines, &ed.text, ed.lang);
    }
    ed.dirty = 1;
}

static void editor_insert_raw(int pos, const char* s, int len) {
    gb_insert_at(&ed.text, pos, s, len);
    if (!lines_apply_insert(&ed.lines, &ed.text, pos, s, len, ed.lang)) {
        lines_rebuild(&ed.lines, &ed.text, ed.lang);
    }
    ed.dirty = 1;
}

static void editor_apply_action(UndoAction a, UndoStack* inverse_target) {
    if (a.type == UNDO_DELETE) {
        char* txt = gb_copy_range(&ed.text, a.pos, a.pos + a.len);
        editor_delete_raw(a.pos, a.pos + a.len);
        ed.cursor = a.pos;
        ed.sel_bound = -1;
        update_pref_col();

        UndoAction inv;
        inv.type = UNDO_INSERT;
        inv.pos = a.pos;
        inv.len = a.len;
        inv.text = txt;
        ustack_push(inverse_target, inv);
        uaction_free(&a);
        return;
    }

    if (a.type == UNDO_INSERT) {
        editor_insert_raw(a.pos, a.text ? a.text : "", a.len);
        ed.cursor = a.pos + a.len;
        ed.sel_bound = -1;
        update_pref_col();

        UndoAction inv;
        inv.type = UNDO_DELETE;
        inv.pos = a.pos;
        inv.len = a.len;
        inv.text = 0;
        ustack_push(inverse_target, inv);
        uaction_free(&a);
        return;
    }

    uaction_free(&a);
}

static void editor_undo(void) {
    UndoAction a = ustack_pop(&ed.undo);
    if (!a.type) return;
    editor_apply_action(a, &ed.redo);
}

static void editor_redo(void) {
    UndoAction a = ustack_pop(&ed.redo);
    if (!a.type) return;
    editor_apply_action(a, &ed.undo);
}

static int gb_match_at(const GapBuf* g, int pos, const char* needle, int nlen) {
    if (nlen <= 0) return 1;
    for (int i = 0; i < nlen; i++) {
        if (gb_char_at(g, pos + i) != needle[i]) return 0;
    }
    return 1;
}

static int gb_find_forward(const GapBuf* g, int start, const char* needle, int nlen) {
    int len = gb_len(g);
    if (nlen <= 0 || !needle) return -1;
    if (start < 0) start = 0;
    if (start > len) start = len;

    for (int i = start; i + nlen <= len; i++) {
        if (gb_match_at(g, i, needle, nlen)) return i;
    }
    return -1;
}

static void mini_set(const char* s, int len) {
    if (len < 0) len = 0;
    if (len > (int)sizeof(ed.mini) - 1) len = (int)sizeof(ed.mini) - 1;
    for (int i = 0; i < len; i++) ed.mini[i] = s[i];
    ed.mini[len] = 0;
    ed.mini_len = len;
}

static void mini_clear(void) {
    ed.mini[0] = 0;
    ed.mini_len = 0;
}

static void mini_backspace(void) {
    if (ed.mini_len <= 0) return;
    ed.mini_len--;
    ed.mini[ed.mini_len] = 0;
    ed.open_confirm = 0;
}

static void mini_putc(char c) {
    if (ed.mini_len >= (int)sizeof(ed.mini) - 1) return;
    ed.mini[ed.mini_len++] = c;
    ed.mini[ed.mini_len] = 0;
    ed.open_confirm = 0;
}

static void editor_update_lang_from_filename(void) {
    ed.lang = LANG_ASM;
    const char* ext = path_ext(ed.filename);
    if (ext) {
        if (ext_eq_ci(ext, "c") || ext_eq_ci(ext, "h")) {
            ed.lang = LANG_C;
        } else if (ext_eq_ci(ext, "asm") || ext_eq_ci(ext, "s") || ext_eq_ci(ext, "inc")) {
            ed.lang = LANG_ASM;
        }
    }
}

static void status_set_col(const char* s, uint32_t col);

static int editor_set_filename(const char* path) {
    if (!path) return 0;
    int n = (int)strlen(path);
    if (n <= 0) return 0;
    if (n >= (int)sizeof(ed.filename)) {
        status_set_col("Path too long", C_UI_ERROR);
        return 0;
    }
    for (int i = 0; i < n; i++) ed.filename[i] = path[i];
    ed.filename[n] = 0;
    return 1;
}

static void status_set(const char* s) {
    if (!s) { ed.status[0] = 0; ed.status_len = 0; ed.status_color = C_UI_MUTED; return; }
    int n = (int)strlen(s);
    if (n > (int)sizeof(ed.status) - 1) n = (int)sizeof(ed.status) - 1;
    for (int i = 0; i < n; i++) ed.status[i] = s[i];
    ed.status[n] = 0;
    ed.status_len = n;
    ed.status_color = C_UI_MUTED;
}

static void status_set_col(const char* s, uint32_t col) {
    status_set(s);
    ed.status_color = col;
}

static int editor_find_next_from(int start) {
    if (ed.find_len <= 0) return 0;
    int pos = gb_find_forward(&ed.text, start, ed.find, ed.find_len);
    if (pos < 0 && start > 0) pos = gb_find_forward(&ed.text, 0, ed.find, ed.find_len);
    if (pos < 0) return 0;
    status_set(0);
    ed.sel_bound = pos;
    ed.cursor = pos + ed.find_len;
    update_pref_col();
    return 1;
}

static void enter_find_mode(void) {
    ed.mode = MODE_FIND;
    if (ed.find_len > 0) mini_set(ed.find, ed.find_len);
    else mini_clear();
}

static void enter_goto_mode(void) {
    ed.mode = MODE_GOTO;
    mini_clear();
}

static void enter_open_mode(void) {
    ed.mode = MODE_OPEN;
    int n = (int)strlen(ed.filename);
    if (n > 0) mini_set(ed.filename, n);
    else mini_clear();
    ed.open_confirm = 0;
}

static void apply_find_mode(void) {
    ed.find_len = ed.mini_len;
    if (ed.find_len > (int)sizeof(ed.find) - 1) ed.find_len = (int)sizeof(ed.find) - 1;
    for (int i = 0; i < ed.find_len; i++) ed.find[i] = ed.mini[i];
    ed.find[ed.find_len] = 0;

    ed.mode = MODE_EDIT;
    if (ed.find_len > 0) {
        int start = ed.cursor;
        int text_len = gb_len(&ed.text);
        if (start < 0) start = 0;
        if (start > text_len) start = text_len;
        if (!editor_find_next_from(start)) status_set_col("Not found", C_UI_ERROR);
    }
}

static void apply_goto_mode(void) {
    int line = atoi(ed.mini);
    ed.mode = MODE_EDIT;

    if (ed.lines.count <= 0) lines_rebuild(&ed.lines, &ed.text, ed.lang);
    if (ed.lines.count <= 0) return;

    if (line < 1) line = 1;
    if (line > ed.lines.count) line = ed.lines.count;

    ed.cursor = ed.lines.starts[line - 1];
    ed.sel_bound = -1;
    update_pref_col();
}

static void apply_open_mode(void) {
    char path[256];
    int n = ed.mini_len;
    if (n < 0) n = 0;
    if (n >= (int)sizeof(path)) n = (int)sizeof(path) - 1;
    for (int i = 0; i < n; i++) path[i] = ed.mini[i];
    path[n] = 0;

    char* p = path;
    while (*p == ' ' || *p == '\t') p++;
    int pn = (int)strlen(p);
    while (pn > 0 && (p[pn - 1] == ' ' || p[pn - 1] == '\t')) {
        p[pn - 1] = 0;
        pn--;
    }
    if (p[0] == 0) {
        status_set_col("Empty path", C_UI_ERROR);
        ed.open_confirm = 0;
        return;
    }

    if (ed.dirty && !ed.open_confirm) {
        status_set_col("Unsaved changes: press Enter again", C_UI_ERROR);
        ed.open_confirm = 1;
        return;
    }

    char old_filename[256];
    int old_fn_len = (int)strlen(ed.filename);
    if (old_fn_len < 0) old_fn_len = 0;
    if (old_fn_len >= (int)sizeof(old_filename)) old_fn_len = (int)sizeof(old_filename) - 1;
    for (int i = 0; i < old_fn_len; i++) old_filename[i] = ed.filename[i];
    old_filename[old_fn_len] = 0;
    const int old_lang = ed.lang;

    if (!editor_set_filename(p)) {
        ed.open_confirm = 0;
        return;
    }
    editor_update_lang_from_filename();

    int ok = load_file();
    if (ok) {
        ed.mode = MODE_EDIT;
        ed.open_confirm = 0;
        return;
    }

    (void)editor_set_filename(old_filename);
    ed.lang = old_lang;
    ed.mode = MODE_OPEN;
    ed.open_confirm = 0;
}

static int editor_insert_with_undo_at(int pos, const char* s, int len) {
    if (len <= 0) return 1;
    ustack_reset(&ed.redo);
    UndoAction ua;
    ua.type = UNDO_DELETE;
    ua.pos = pos;
    ua.len = len;
    ua.text = 0;
    if (!ustack_push(&ed.undo, ua)) return 0;
    editor_insert_raw(pos, s, len);
    return 1;
}

static int count_line_indent_spaces(int line_start) {
    int text_len = gb_len(&ed.text);
    int i = line_start;
    int spaces = 0;
    while (i < text_len) {
        char c = gb_char_at(&ed.text, i);
        if (c == ' ') { spaces++; i++; }
        else if (c == '\t') { spaces += 4; i++; }
        else break;
        if (spaces > 60) { spaces = 60; break; }
    }
    return spaces;
}

static int is_asm_label_line(int line_start) {
    int text_len = gb_len(&ed.text);
    int line_len = get_line_len(line_start);
    int line_end = line_start + line_len;
    if (line_end > text_len) line_end = text_len;

    int i = line_start;
    while (i < line_end && (gb_char_at(&ed.text, i) == ' ' || gb_char_at(&ed.text, i) == '\t')) i++;
    if (i >= line_end) return 0;

    int start = i;
    while (i < line_end) {
        char c = gb_char_at(&ed.text, i);
        if (c == ';') break;
        if (c == ':') {
            return i > start;
        }
        if (c == ' ' || c == '\t') return 0;
        if (!is_word_char(c)) return 0;
        i++;
    }
    return 0;
}

static int last_nonspace_before_in_line(int line_start, int pos) {
    int i = pos - 1;
    while (i >= line_start) {
        char c = gb_char_at(&ed.text, i);
        if (c != ' ' && c != '\t' && c != '\r') return (unsigned char)c;
        i--;
    }
    return 0;
}

static int next_nonspace_after_in_line(int pos, int line_end) {
    int i = pos;
    while (i < line_end) {
        char c = gb_char_at(&ed.text, i);
        if (c != ' ' && c != '\t' && c != '\r') return (unsigned char)c;
        i++;
    }
    return 0;
}

static void editor_insert_newline_autoindent(void) {
    if (ed.sel_bound != -1) {
        int s = min(ed.sel_bound, ed.cursor);
        int e = max(ed.sel_bound, ed.cursor);
        delete_range(s, e);
    }

    int line_start = get_line_start(ed.cursor);
    int indent = count_line_indent_spaces(line_start);

    int line_len = get_line_len(line_start);
    int line_end = line_start + line_len;

    if (ed.lang == LANG_C) {
        int last = last_nonspace_before_in_line(line_start, ed.cursor);
        if (last == '{') {
            int inner = indent + 4;
            if (inner > 60) inner = 60;

            int next = next_nonspace_after_in_line(ed.cursor, line_end);
            if (next == '}') {
                char buf[2 + 2 * 64];
                int n = 0;
                buf[n++] = '\n';
                for (int i = 0; i < inner; i++) buf[n++] = ' ';
                buf[n++] = '\n';
                for (int i = 0; i < indent; i++) buf[n++] = ' ';

                if (!editor_insert_with_undo_at(ed.cursor, buf, n)) return;
                ed.cursor += 1 + inner;
                ed.sel_bound = -1;
                update_pref_col();
                return;
            }

            indent = inner;
        } else {
            int next = next_nonspace_after_in_line(ed.cursor, line_end);
            if (next == '}') {
                int inner = indent + 4;
                if (inner > 60) inner = 60;
                char buf[2 + 2 * 64];
                int n = 0;
                buf[n++] = '\n';
                for (int i = 0; i < inner; i++) buf[n++] = ' ';
                buf[n++] = '\n';
                for (int i = 0; i < indent; i++) buf[n++] = ' ';

                if (!editor_insert_with_undo_at(ed.cursor, buf, n)) return;
                ed.cursor += 1 + inner;
                ed.sel_bound = -1;
                update_pref_col();
                return;
            }
        }
    } else {
        if (is_asm_label_line(line_start)) indent = 4;
    }

    if (indent < 0) indent = 0;
    if (indent > 60) indent = 60;

    char buf[1 + 64];
    buf[0] = '\n';
    for (int i = 0; i < indent; i++) buf[1 + i] = ' ';
    int len = 1 + indent;

    if (!editor_insert_with_undo_at(ed.cursor, buf, len)) return;
    ed.cursor += len;
    ed.sel_bound = -1;
    update_pref_col();
}

static void editor_insert_tab_smart(void) {
    if (ed.sel_bound != -1) {
        int s = min(ed.sel_bound, ed.cursor);
        int e = max(ed.sel_bound, ed.cursor);
        delete_range(s, e);
    }

    int line_start = get_line_start(ed.cursor);
    int col = ed.cursor - line_start;
    if (col < 0) col = 0;
    int add = 4 - (col % 4);
    if (add <= 0) add = 4;
    if (add > 16) add = 4;

    char spaces[16];
    for (int i = 0; i < add; i++) spaces[i] = ' ';
    editor_insert_with_undo_at(ed.cursor, spaces, add);
    ed.cursor += add;
    ed.sel_bound = -1;
    update_pref_col();
}

void fmt_int(int n, char* buf) {
    if (n == 0) { buf[0]='0'; buf[1]=0; return; }
    int i=0, t=n;
    while(t>0) { t/=10; i++; }
    buf[i]=0;
    while(n>0) { buf[--i]=(n%10)+'0'; n/=10; }
}

void draw_rect(int x, int y, int w, int h, uint32_t color) {
    for(int j=0; j<h; j++) {
        int py = y+j;
        if(py >= WIN_H) break; if(py < 0) continue;
        for(int i=0; i<w; i++) {
            int px = x+i;
            if(px >= WIN_W) break; if(px < 0) continue;
            canvas[py*WIN_W + px] = color;
        }
    }
}

void render_char(int x, int y, char c, uint32_t color) {
    draw_char(canvas, WIN_W, WIN_H, x, y + 5, c, color);
}

void render_string(int x, int y, const char* s, uint32_t color) {
    while(*s) {
        render_char(x, y, *s, color);
        x += CHAR_W;
        s++;
    }
}

int get_line_start(int pos) {
    if (ed.lines.count <= 0) lines_rebuild(&ed.lines, &ed.text, ed.lang);
    int line = lines_find_line(&ed.lines, pos);
    if (line < 0) line = 0;
    if (line >= ed.lines.count) line = ed.lines.count - 1;
    if (line < 0) return 0;
    return ed.lines.starts[line];
}

int get_line_len(int start) {
    int text_len = gb_len(&ed.text);
    if (ed.lines.count <= 0) lines_rebuild(&ed.lines, &ed.text, ed.lang);
    int line = lines_find_line(&ed.lines, start);
    if (line < 0) line = 0;
    if (line >= ed.lines.count) line = ed.lines.count - 1;
    int line_start = (line >= 0 && line < ed.lines.count) ? ed.lines.starts[line] : 0;
    int line_end = (line + 1 < ed.lines.count) ? (ed.lines.starts[line + 1] - 1) : text_len;
    if (line_end < line_start) line_end = line_start;
    return line_end - line_start;
}

void update_pref_col() {
    ed.pref_col = ed.cursor - get_line_start(ed.cursor);
}

void delete_range(int start, int end) {
    if (start >= end) return;

    int len = end - start;
    char* deleted = gb_copy_range(&ed.text, start, end);
    if (!deleted) return;

    ustack_reset(&ed.redo);
    UndoAction ua;
    ua.type = UNDO_INSERT;
    ua.pos = start;
    ua.len = len;
    ua.text = deleted;
    if (!ustack_push(&ed.undo, ua)) return;

    editor_delete_raw(start, end);

    ed.cursor = start;
    ed.sel_bound = -1;
    update_pref_col();
}

void insert_str(const char* s, int len) {
    if (ed.sel_bound != -1) {
        int mn = min(ed.sel_bound, ed.cursor);
        int mx = max(ed.sel_bound, ed.cursor);
        delete_range(mn, mx);
    }

    if (!editor_insert_with_undo_at(ed.cursor, s, len)) return;
    ed.cursor += len;
    update_pref_col();
}

void insert_char(char c) {
    char buf[1] = {c};
    insert_str(buf, 1);
}

void backspace() {
    if (ed.sel_bound != -1) {
        int s = min(ed.sel_bound, ed.cursor);
        int e = max(ed.sel_bound, ed.cursor);
        delete_range(s, e);
    } else if (ed.cursor > 0) {
        delete_range(ed.cursor - 1, ed.cursor);
    }
}

void copy_selection() {
    if (ed.sel_bound == -1 || ed.sel_bound == ed.cursor) return;

    int s = min(ed.sel_bound, ed.cursor);
    int e = max(ed.sel_bound, ed.cursor);

    char* tmp = gb_copy_range(&ed.text, s, e);
    if (!tmp) return;

    clipboard_copy(tmp);
    free(tmp);
}

void paste_clipboard() {
    char* buf = malloc(4096);
    if (!buf) return;
    int len = clipboard_paste(buf, 4096);
    if (len > 0) {
        insert_str(buf, len);
    }
    free(buf);
}

void handle_selection(int select) {
    if (select) {
        if (ed.sel_bound == -1) ed.sel_bound = ed.cursor;
    } else {
        ed.sel_bound = -1;
    }
}

void move_left(int select) {
    handle_selection(select);
    if (ed.cursor > 0) ed.cursor--;
    update_pref_col();
}

void move_right(int select) {
    handle_selection(select);
    int len = gb_len(&ed.text);
    if (ed.cursor < len) ed.cursor++;
    update_pref_col();
}

void move_up(int select) {
    handle_selection(select);
    int curr_start = get_line_start(ed.cursor);
    if (curr_start == 0) {
        ed.cursor = 0;
    } else {
        int prev_start = get_line_start(curr_start - 1);
        int prev_len = get_line_len(prev_start);
        int target = min(prev_len, ed.pref_col);
        ed.cursor = prev_start + target;
    }
}

void move_down(int select) {
    handle_selection(select);
    int curr_start = get_line_start(ed.cursor);
    int curr_len = get_line_len(curr_start);
    int next_start = curr_start + curr_len + 1;
    int len = gb_len(&ed.text);
    if (next_start < len) {
        int next_len = get_line_len(next_start);
        int target = min(next_len, ed.pref_col);
        ed.cursor = next_start + target;
    } else {
        ed.cursor = len;
    }
}

void move_word_left(int select) {
    handle_selection(select);
    if (ed.cursor == 0) return;
    ed.cursor--;
    while (ed.cursor > 0 && !is_word_char(gb_char_at(&ed.text, ed.cursor))) ed.cursor--;
    while (ed.cursor > 0 && is_word_char(gb_char_at(&ed.text, ed.cursor - 1))) ed.cursor--;
    update_pref_col();
}

void move_word_right(int select) {
    handle_selection(select);
    int len = gb_len(&ed.text);
    if (ed.cursor >= len) return;
    while (ed.cursor < len && is_word_char(gb_char_at(&ed.text, ed.cursor))) ed.cursor++;
    while (ed.cursor < len && !is_word_char(gb_char_at(&ed.text, ed.cursor))) ed.cursor++;
    update_pref_col();
}

static int load_file(void) {
    return load_file_impl(1);
}

static int load_file_silent(void) {
    return load_file_impl(0);
}

static int load_file_impl(int show_status) {
    int fd = open(ed.filename, 0);
    if (fd < 0) {
        if (show_status) status_set_col("Open failed", C_UI_ERROR);
        return 0;
    }

    GapBuf new_text;
    gb_init(&new_text, 4096);
    if (!new_text.buf) {
        close(fd);
        if (show_status) status_set_col("Out of memory", C_UI_ERROR);
        return 0;
    }

    int ok = 1;
    char* tmp = (char*)malloc(8192);
    if (!tmp) ok = 0;

    if (ok) {
        int r = 0;
        while ((r = read(fd, tmp, 8192)) > 0) {
            if (!gb_insert_at(&new_text, gb_len(&new_text), tmp, r)) {
                ok = 0;
                break;
            }
        }
        if (r < 0) ok = 0;
    }

    if (tmp) free(tmp);
    close(fd);

    if (!ok) {
        gb_destroy(&new_text);
        if (show_status) status_set_col("Open failed", C_UI_ERROR);
        return 0;
    }

    GapBuf old_text = ed.text;
    ed.text = new_text;
    gb_destroy(&old_text);

    lines_rebuild(&ed.lines, &ed.text, ed.lang);
    ed.cursor = 0;
    ed.sel_bound = -1;
    ed.scroll_y = 0;
    ed.dirty = 0;
    update_pref_col();

    ustack_reset(&ed.undo);
    ustack_reset(&ed.redo);

    if (show_status) status_set_col("Opened", C_UI_OK);
    return 1;
}

void save_file() {
    int fd = open(ed.filename, 1);
    if (fd < 0) {
        status_set_col("Save failed", C_UI_ERROR);
        return;
    }

    int ok = 1;
    if (ed.text.gap_start > 0) {
        int w = write(fd, ed.text.buf, ed.text.gap_start);
        if (w != ed.text.gap_start) ok = 0;
    }
    int after = ed.text.cap - ed.text.gap_end;
    if (ok && after > 0) {
        int w = write(fd, ed.text.buf + ed.text.gap_end, after);
        if (w != after) ok = 0;
    }
    close(fd);

    if (ok) {
        ed.dirty = 0;
        status_set_col("Saved", C_UI_OK);
    } else {
        status_set_col("Save failed", C_UI_ERROR);
    }
}

int get_pos_from_coords(int mx, int my) {
    int row = (my - TAB_H) / LINE_H;
    int target_line = ed.scroll_y + row;

    if (ed.lines.count <= 0) lines_rebuild(&ed.lines, &ed.text, ed.lang);
    int text_len = gb_len(&ed.text);
    if (target_line < 0) target_line = 0;
    if (target_line >= ed.lines.count) return text_len;

    int line_start = ed.lines.starts[target_line];
    int line_end = (target_line + 1 < ed.lines.count) ? (ed.lines.starts[target_line + 1] - 1) : text_len;
    if (line_end < line_start) line_end = line_start;
    int len = line_end - line_start;

    int click_x = mx - (GUTTER_W + PAD_X);
    int col = (click_x + (CHAR_W/2)) / CHAR_W;
    if (col < 0) col = 0;
    if (col > len) col = len;
    return line_start + col;
}

int check_kw(const char* text, int len, const char** list) {
    for(int i=0; list[i]; i++) {
        int l = strlen(list[i]);
        if (l == len && strncmp(text, list[i], len) == 0) return 1;
    }
    return 0;
}

int check_kw_gb(int pos, int len, const char** list) {
    for (int i = 0; list[i]; i++) {
        int l = strlen(list[i]);
        if (l != len) continue;
        int ok = 1;
        for (int k = 0; k < len; k++) {
            if (gb_char_at(&ed.text, pos + k) != list[i][k]) { ok = 0; break; }
        }
        if (ok) return 1;
    }
    return 0;
}

void render_editor() {
    int view_lines = (WIN_H - STATUS_H - TAB_H) / LINE_H;
    if (ed.lines.count <= 0) lines_rebuild(&ed.lines, &ed.text, ed.lang);

    int cur_line = lines_find_line(&ed.lines, ed.cursor);
    int text_len = gb_len(&ed.text);
    
    if (cur_line < ed.scroll_y) ed.scroll_y = cur_line;
    if (cur_line >= ed.scroll_y + view_lines) ed.scroll_y = cur_line - view_lines + 1;

    draw_rect(0, TAB_H, WIN_W, WIN_H - STATUS_H - TAB_H, C_BG);
    draw_rect(0, TAB_H, GUTTER_W, WIN_H - STATUS_H - TAB_H, C_GUTTER_BG);
    draw_rect(GUTTER_W - 1, TAB_H, 1, WIN_H - STATUS_H - TAB_H, C_UI_BORDER);

    if (ed.scroll_y < 0) ed.scroll_y = 0;
    if (ed.lines.count > 0 && ed.scroll_y >= ed.lines.count) ed.scroll_y = ed.lines.count - 1;

    int line_idx = ed.scroll_y;

    int draw_y = TAB_H + 2;
    int s_min = -1, s_max = -1;
    if (ed.sel_bound != -1) {
        s_min = min(ed.sel_bound, ed.cursor);
        s_max = max(ed.sel_bound, ed.cursor);
    }

    while (line_idx < ed.lines.count && draw_y < WIN_H - STATUS_H) {
        int line_start = ed.lines.starts[line_idx];
        int line_end = (line_idx + 1 < ed.lines.count) ? (ed.lines.starts[line_idx + 1] - 1) : text_len;
        if (line_end < line_start) line_end = line_start;
        int line_len = line_end - line_start;
        int is_active = (cur_line == line_idx);

        if (is_active) draw_rect(GUTTER_W, draw_y, WIN_W - GUTTER_W, LINE_H, C_ACTIVE_LINE);

        char num[12]; fmt_int(line_idx + 1, num);
        int num_w = strlen(num)*8;
        render_string(GUTTER_W - 8 - num_w, draw_y, num, is_active ? C_GUTTER_FG : 0x505050);

        int draw_x = GUTTER_W + PAD_X;
        int i = 0;
        
        int in_block = (ed.lang == LANG_C && ed.lines.c_block && line_idx < ed.lines.count) ? (ed.lines.c_block[line_idx] ? 1 : 0) : 0;

        while (i < line_len) {
            int abs_pos = line_start + i;
            char c = gb_char_at(&ed.text, abs_pos);
            uint32_t fg = C_TEXT;
            
            int is_sel = (ed.sel_bound != -1 && abs_pos >= s_min && abs_pos < s_max);
            if (is_sel) draw_rect(draw_x, draw_y, CHAR_W, LINE_H, C_SELECTION);
            if (abs_pos == ed.cursor) draw_rect(draw_x, draw_y, 2, LINE_H, C_CURSOR);

            if (ed.lang == LANG_ASM && c == ';') {
                fg = C_SYN_COMMENT;
                while (i < line_len) {
                    abs_pos = line_start + i;
                    is_sel = (ed.sel_bound != -1 && abs_pos >= s_min && abs_pos < s_max);
                    if (is_sel) draw_rect(draw_x, draw_y, CHAR_W, LINE_H, C_SELECTION);
                    if (abs_pos == ed.cursor) draw_rect(draw_x, draw_y, 2, LINE_H, C_CURSOR);
                    render_char(draw_x, draw_y, gb_char_at(&ed.text, abs_pos), fg);
                    draw_x += CHAR_W; i++;
                }
                break;
            }

            if (ed.lang == LANG_C) {
                char n1 = (i + 1 < line_len) ? gb_char_at(&ed.text, abs_pos + 1) : 0;
                if (!in_block && c == '/' && n1 == '/') {
                    fg = C_SYN_COMMENT;
                    while (i < line_len) {
                        abs_pos = line_start + i;
                        is_sel = (ed.sel_bound != -1 && abs_pos >= s_min && abs_pos < s_max);
                        if (is_sel) draw_rect(draw_x, draw_y, CHAR_W, LINE_H, C_SELECTION);
                        if (abs_pos == ed.cursor) draw_rect(draw_x, draw_y, 2, LINE_H, C_CURSOR);
                        render_char(draw_x, draw_y, gb_char_at(&ed.text, abs_pos), fg);
                        draw_x += CHAR_W; i++;
                    }
                    break;
                }
                if (!in_block && c == '/' && n1 == '*') {
                    in_block = 1;
                }
                if (in_block) {
                    fg = C_SYN_COMMENT;
                    render_char(draw_x, draw_y, c, fg);
                    draw_x += CHAR_W; i++;
                    if (c == '*' && n1 == '/') {
                        is_sel = (ed.sel_bound != -1 && abs_pos + 1 >= s_min && abs_pos + 1 < s_max);
                        if (is_sel) draw_rect(draw_x, draw_y, CHAR_W, LINE_H, C_SELECTION);
                        if (abs_pos + 1 == ed.cursor) draw_rect(draw_x, draw_y, 2, LINE_H, C_CURSOR);
                        render_char(draw_x, draw_y, '/', fg);
                        draw_x += CHAR_W; i++;
                        in_block = 0;
                    }
                    continue;
                }
            }

            if (c == '"' || c == '\'') {
                char q = c;
                fg = C_SYN_STRING;
                render_char(draw_x, draw_y, c, fg);
                draw_x += CHAR_W; i++;
                while (i < line_len) {
                    abs_pos = line_start + i;
                    char cc = gb_char_at(&ed.text, abs_pos);
                    is_sel = (ed.sel_bound != -1 && abs_pos >= s_min && abs_pos < s_max);
                    if (is_sel) draw_rect(draw_x, draw_y, CHAR_W, LINE_H, C_SELECTION);
                    if (abs_pos == ed.cursor) draw_rect(draw_x, draw_y, 2, LINE_H, C_CURSOR);
                    render_char(draw_x, draw_y, cc, fg);
                    draw_x += CHAR_W; i++;
                    if (cc == q) break;
                }
                continue;
            }

            if (is_word_char(c) && (i==0 || !is_word_char(gb_char_at(&ed.text, line_start + i - 1)))) {
                int wlen = 0;
                while (i+wlen < line_len && is_word_char(gb_char_at(&ed.text, line_start + i + wlen))) wlen++;
                
                if (ed.lang == LANG_ASM) {
                    if (check_kw_gb(abs_pos, wlen, kwd_general)) fg = C_SYN_KEYWORD;
                    else if (check_kw_gb(abs_pos, wlen, kwd_control)) fg = C_SYN_CONTROL;
                    else if (check_kw_gb(abs_pos, wlen, kwd_dirs)) fg = C_SYN_DIRECTIVE;
                    else if (check_kw_gb(abs_pos, wlen, kwd_regs)) fg = C_SYN_REG;
                    else if (is_digit(c)) fg = C_SYN_NUMBER;
                } else {
                    if (check_kw_gb(abs_pos, wlen, c_kwd_types)) fg = C_SYN_KEYWORD;
                    else if (check_kw_gb(abs_pos, wlen, c_kwd_ctrl)) fg = C_SYN_CONTROL;
                    else if (line_start == 0 || gb_char_at(&ed.text, line_start) == '#') {
                        if (check_kw_gb(abs_pos, wlen, c_kwd_pp)) fg = C_SYN_DIRECTIVE;
                    }
                    else if (is_digit(c)) fg = C_SYN_NUMBER;
                }
                
                for (int k=0; k<wlen; k++) {
                    abs_pos = line_start + i;
                    is_sel = (ed.sel_bound != -1 && abs_pos >= s_min && abs_pos < s_max);
                    if (is_sel) draw_rect(draw_x, draw_y, CHAR_W, LINE_H, C_SELECTION);
                    if (abs_pos == ed.cursor) draw_rect(draw_x, draw_y, 2, LINE_H, C_CURSOR);
                    render_char(draw_x, draw_y, gb_char_at(&ed.text, abs_pos), fg);
                    draw_x += CHAR_W; i++;
                }
                continue;
            }

            render_char(draw_x, draw_y, c, fg);
            draw_x += CHAR_W; i++;
        }

        if (line_start + line_len == ed.cursor) draw_rect(draw_x, draw_y, 2, LINE_H, C_CURSOR);
        line_idx++;
        draw_y += LINE_H;
    }
}

void render_ui() {
    draw_rect(0, 0, WIN_W, TAB_H, C_GUTTER_BG);
    draw_rect(0, TAB_H - 1, WIN_W, 1, C_UI_BORDER);

    const char* base = path_base(ed.filename);
    int max_chars = (WIN_W - 220) / CHAR_W;
    if (max_chars > 40) max_chars = 40;
    if (max_chars < 10) max_chars = 10;
    char title[64];
    fmt_title_ellipsis(base, title, (int)sizeof(title), max_chars);

    const char* lang = (ed.lang == LANG_C) ? "C" : "ASM";
    int title_w = (int)strlen(title) * CHAR_W;
    int lang_w = (int)strlen(lang) * CHAR_W;
    int tab_w = 16 + title_w + 12 + 8 + lang_w + 16;
    if (ed.dirty) tab_w += CHAR_W;
    if (tab_w > WIN_W - 160) tab_w = WIN_W - 160;
    if (tab_w < 120) tab_w = 120;

    draw_rect(0, 0, tab_w, TAB_H, C_TAB_BG);
    draw_rect(0, TAB_H - 2, tab_w, 2, C_UI_ACCENT);

    int tx = 12;
    int ty = 8;
    render_string(tx, ty, title, C_TAB_FG);
    tx += title_w;
    int y = WIN_H - STATUS_H;
    draw_rect(0, y, WIN_W, STATUS_H, C_STATUS_BG);
    draw_rect(0, y, WIN_W, 1, C_UI_BORDER);
    int status_text_y = y + 6;
    
    if (ed.lines.count <= 0) lines_rebuild(&ed.lines, &ed.text, ed.lang);
    int line0 = lines_find_line(&ed.lines, ed.cursor);
    int line_start = (line0 >= 0 && line0 < ed.lines.count) ? ed.lines.starts[line0] : 0;
    int line = line0 + 1;
    int col = (ed.cursor - line_start) + 1;
    
    char l[8], c[8]; fmt_int(line, l); fmt_int(col, c);
 
    if (ed.mode == MODE_EDIT) {
        render_string(WIN_W - 190, status_text_y, (ed.lang == LANG_C) ? "C" : "ASM", C_UI_MUTED);
        render_string(WIN_W - 150, status_text_y, "Ln", C_UI_MUTED);
        render_string(WIN_W - 130, status_text_y, l, C_STATUS_FG);
        render_string(WIN_W - 80, status_text_y, "Col", C_UI_MUTED);
        render_string(WIN_W - 50, status_text_y, c, C_STATUS_FG);
    } else if (ed.status_len > 0) {
        int st_max_chars = (210 - 20) / CHAR_W;
        if (st_max_chars < 4) st_max_chars = 4;
        char disp_status[64];
        fmt_title_ellipsis(ed.status, disp_status, (int)sizeof(disp_status), st_max_chars);
        render_string(WIN_W - 210 + 10, status_text_y, disp_status, ed.status_color ? ed.status_color : C_UI_MUTED);
    }

    if (ed.mode == MODE_FIND) {
        int right_w = 210;
        int px = 10;
        render_string(px, status_text_y, "Find:", C_UI_MUTED);
        int bx = px + 6 * CHAR_W + 8;
        int bw = (WIN_W - right_w) - bx - 10;
        if (bw < 80) bw = 80;

        int text_y = status_text_y;
        int glyph_top = text_y + 5;
        int pad_y = 2;
        int box_shift = 1;
        int by = glyph_top - pad_y + box_shift;
        int bh = 8 + pad_y * 2;
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
        int ch = 8;
        draw_rect(cx, cy, 2, ch, C_CURSOR);
    }
    else if (ed.mode == MODE_GOTO) {
        int right_w = 210;
        int px = 10;
        render_string(px, status_text_y, "Goto:", C_UI_MUTED);
        int bx = px + 6 * CHAR_W + 8;
        int bw = (WIN_W - right_w) - bx - 10;
        if (bw < 80) bw = 80;

        int text_y = status_text_y;
        int glyph_top = text_y + 5;
        int pad_y = 2;
        int box_shift = 1;
        int by = glyph_top - pad_y + box_shift;
        int bh = 8 + pad_y * 2;
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
        int ch = 8;
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
        int glyph_top = text_y + 5;
        int pad_y = 2;
        int box_shift = 1;
        int by = glyph_top - pad_y + box_shift;
        int bh = 8 + pad_y * 2;
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
        int ch = 8;
        draw_rect(cx, cy, 2, ch, C_CURSOR);
    }
    else if (ed.status_len > 0) {
        render_string(10, status_text_y, ed.status, ed.status_color ? ed.status_color : C_UI_MUTED);
    }
}

static int ensure_surface(uint32_t need_w, uint32_t need_h) {
    if (need_w == 0 || need_h == 0) return -1;

    uint64_t bytes64 = (uint64_t)need_w * (uint64_t)need_h * 4ull;
    if (bytes64 == 0 || bytes64 > 0xFFFFFFFFu) return -1;
    const uint32_t need_bytes = (uint32_t)bytes64;

    const int can_reuse = (canvas && shm_fd >= 0 && shm_name[0] != '\0' && need_bytes <= size_bytes);
    if (can_reuse) {
        uint16_t err = 0;
        if (comp_send_attach_shm_name_sync(&conn, surface_id, shm_name, size_bytes, need_w, need_h, need_w, 0u, 2000u, &err) != 0) {
            return -1;
        }
        return 0;
    }

    uint64_t grow64 = (uint64_t)size_bytes * 2ull;
    uint64_t cap64 = (grow64 >= (uint64_t)need_bytes) ? grow64 : (uint64_t)need_bytes;
    if (cap64 > 0xFFFFFFFFu) cap64 = (uint64_t)need_bytes;
    const uint32_t cap_bytes = (uint32_t)cap64;

    char new_name[32];
    new_name[0] = '\0';
    int new_fd = -1;
    for (int i = 0; i < 16; i++) {
        shm_gen++;
        (void)snprintf(new_name, sizeof(new_name), "geditor_%d_r%d", getpid(), shm_gen);
        new_fd = shm_create_named(new_name, cap_bytes);
        if (new_fd >= 0) break;
    }
    if (new_fd < 0) return -1;

    uint32_t* new_canvas = (uint32_t*)mmap(new_fd, cap_bytes, MAP_SHARED);
    if (!new_canvas) {
        close(new_fd);
        shm_unlink_named(new_name);
        return -1;
    }

    uint16_t err = 0;
    if (comp_send_attach_shm_name_sync(&conn, surface_id, new_name, cap_bytes, need_w, need_h, need_w, 0u, 2000u, &err) != 0) {
        munmap((void*)new_canvas, cap_bytes);
        close(new_fd);
        shm_unlink_named(new_name);
        return -1;
    }

    uint32_t* old_canvas = canvas;
    uint32_t old_size_bytes = size_bytes;
    int old_fd = shm_fd;
    char old_name[32];
    memcpy(old_name, shm_name, sizeof(old_name));

    canvas = new_canvas;
    size_bytes = cap_bytes;
    shm_fd = new_fd;
    memcpy(shm_name, new_name, sizeof(shm_name));

    if (old_canvas) munmap((void*)old_canvas, old_size_bytes);
    if (old_fd >= 0) close(old_fd);
    if (old_name[0]) shm_unlink_named(old_name);
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) strcpy(ed.filename, "new.asm");
    else strcpy(ed.filename, argv[1]);

    set_term_mode(0);

    editor_update_lang_from_filename();

    gb_init(&ed.text, 4096);
    lines_init(&ed.lines);
    if (!ed.text.buf) return 1;

    ustack_init(&ed.undo);
    ustack_init(&ed.redo);
    
    ed.sel_bound = -1;
    ed.dirty = 0;
    ed.is_dragging = 0;
    ed.scroll_y = 0;
    ed.cursor = 0;
    ed.pref_col = 0;
    ed.mode = MODE_EDIT;
    ed.mini_len = 0;
    ed.open_confirm = 0;

    ed.find_len = 0;
    ed.find[0] = 0;

    ed.status_len = 0;
    ed.status[0] = 0;
    ed.status_color = C_UI_MUTED;
    
    (void)load_file_silent();

    comp_conn_reset(&conn);
    if (comp_connect(&conn, "compositor") != 0) return 1;
    if (comp_send_hello(&conn) != 0) {
        comp_disconnect(&conn);
        return 1;
    }

    shm_name[0] = '\0';
    size_bytes = (uint32_t)WIN_W * (uint32_t)WIN_H * 4u;
    for (int i = 0; i < 8; i++) {
        (void)snprintf(shm_name, sizeof(shm_name), "geditor_%d_%d", getpid(), i);
        shm_fd = shm_create_named(shm_name, size_bytes);
        if (shm_fd >= 0) break;
    }
    if (shm_fd < 0) {
        comp_disconnect(&conn);
        return 1;
    }

    canvas = (uint32_t*)mmap(shm_fd, size_bytes, MAP_SHARED);
    if (!canvas) {
        close(shm_fd);
        shm_fd = -1;
        shm_unlink_named(shm_name);
        shm_name[0] = '\0';
        comp_disconnect(&conn);
        return 1;
    }

    {
        uint16_t err = 0;
        if (comp_send_attach_shm_name_sync(&conn, surface_id, shm_name, size_bytes, (uint32_t)WIN_W, (uint32_t)WIN_H, (uint32_t)WIN_W, 0u, 2000u, &err) != 0) {
            munmap((void*)canvas, size_bytes);
            canvas = 0;
            close(shm_fd);
            shm_fd = -1;
            shm_unlink_named(shm_name);
            shm_name[0] = '\0';
            comp_disconnect(&conn);
            return 1;
        }
    }

    render_editor();
    render_ui();
    if (comp_send_commit(&conn, surface_id, 32, 32, 0u) != 0) {
        (void)comp_send_destroy_surface(&conn, surface_id, 0u);
        munmap((void*)canvas, size_bytes);
        canvas = 0;
        close(shm_fd);
        shm_fd = -1;
        shm_unlink_named(shm_name);
        shm_name[0] = '\0';
        comp_disconnect(&conn);
        return 1;
    }

    comp_ipc_hdr_t hdr;
    uint8_t payload[COMP_IPC_MAX_PAYLOAD];

    int have_mouse = 0;
    int last_mx = 0;
    int last_my = 0;
    int last_buttons = 0;

    while (!ed.quit) {
        int update = 0;
        for (;;) {
            int rr = comp_try_recv(&conn, &hdr, payload, (uint32_t)sizeof(payload));
            if (rr < 0) {
                ed.quit = 1;
                break;
            }
            if (rr == 0) break;

            if (hdr.type != (uint16_t)COMP_IPC_MSG_INPUT || hdr.len != (uint32_t)sizeof(comp_ipc_input_t)) {
                continue;
            }

            comp_ipc_input_t in;
            memcpy(&in, payload, sizeof(in));
            if (in.surface_id != surface_id) continue;

            if (in.kind == COMP_IPC_INPUT_KEY) {
                if (in.key_state != 1u) continue;

                unsigned char c = (unsigned char)(uint8_t)in.keycode;
                if (c == 0x15) { save_file(); update = 1; continue; } // Ctrl+S
                if (c == 0x1A) { editor_undo(); update = 1; continue; } // Ctrl+Z
                if (c == 0x19) { editor_redo(); update = 1; continue; } // Ctrl+Y

                if (ed.mode != MODE_EDIT) {
                    if (c == 0x1B) { ed.mode = MODE_EDIT; ed.open_confirm = 0; update = 1; continue; }
                    if (c == 0x08) { mini_backspace(); update = 1; continue; }
                    if (c == 0x0A || c == 0x0D) {
                        if (ed.mode == MODE_FIND) apply_find_mode();
                        else if (ed.mode == MODE_GOTO) apply_goto_mode();
                        else if (ed.mode == MODE_OPEN) apply_open_mode();
                        update = 1;
                        continue;
                    }
                    if (c >= 32 && c <= 126) { mini_putc((char)c); update = 1; continue; }
                    continue;
                }
                
                if (c == 0x11) move_left(0);
                else if (c == 0x12) move_right(0);
                else if (c == 0x13) move_up(0);
                else if (c == 0x14) move_down(0);
                
                else if (c == 0x82) move_left(1);
                else if (c == 0x83) move_right(1);
                else if (c == 0x80) move_up(1);
                else if (c == 0x81) move_down(1);
                
                // Ctrl + Arrows
                else if (c == 0x84) move_word_left(0);
                else if (c == 0x85) move_word_right(0);
                else if (c == 0x86) move_word_left(1);
                else if (c == 0x87) move_word_right(1);
                
                else if (c == 0x08) backspace();
                else if (c == 0x0A || c == 0x0D) editor_insert_newline_autoindent();
                else if (c == 0x09) editor_insert_tab_smart();
                else if (c == 0x03) copy_selection(); // Ctrl+C
                else if (c == 0x16) paste_clipboard(); // Ctrl+V

                else if (c == 0x06) enter_find_mode(); // Ctrl+F
                else if (c == 0x07) enter_goto_mode(); // Ctrl+G
                else if (c == 0x0F) enter_open_mode();
                else if (c == 0x0E) {
                    if (ed.find_len > 0) {
                        int start = ed.cursor;
                        int text_len = gb_len(&ed.text);
                        if (start < 0) start = 0;
                        if (start > text_len) start = text_len;
                        if (!editor_find_next_from(start)) status_set("Not found");
                    } else {
                        enter_find_mode();
                    }
                } // Ctrl+N
                
                else if (c >= 32 && c <= 126) insert_char((char)c);
                
                update = 1;
                continue;
            }

            if (in.kind == COMP_IPC_INPUT_CLOSE) {
                ed.quit = 1;
                update = 1;
                break;
            }

            if (in.kind == COMP_IPC_INPUT_MOUSE) {
                const int mx = (int)in.x;
                const int my = (int)in.y;
                const int buttons = (int)in.buttons;

                const int prev_buttons = have_mouse ? last_buttons : 0;
                if (!have_mouse) {
                    last_mx = mx;
                    last_my = my;
                    have_mouse = 1;
                }

                const int down_now = ((buttons & 1) != 0);
                const int down_prev = ((prev_buttons & 1) != 0);

                if (down_now && !down_prev) {
                    int pos = get_pos_from_coords(mx, my);
                    ed.cursor = pos;
                    ed.sel_bound = pos;
                    ed.is_dragging = 1;
                    update_pref_col();
                    update = 1;
                }
                if (down_now && ed.is_dragging && (mx != last_mx || my != last_my)) {
                    int pos = get_pos_from_coords(mx, my);
                    if (pos != ed.cursor) {
                        ed.cursor = pos;
                        update_pref_col();
                        update = 1;
                    }
                }
                if (!down_now && down_prev) {
                    ed.is_dragging = 0;
                    if (ed.sel_bound == ed.cursor) ed.sel_bound = -1;
                    update = 1;
                }

                last_mx = mx;
                last_my = my;
                last_buttons = buttons;
                continue;
            }

            if (in.kind == COMP_IPC_INPUT_RESIZE) {
                const int32_t nw = in.x;
                const int32_t nh = in.y;
                if (nw <= 0 || nh <= 0) continue;
                if (nw == WIN_W && nh == WIN_H) continue;

                if (ensure_surface((uint32_t)nw, (uint32_t)nh) != 0) {
                    continue;
                }

                WIN_W = (int)nw;
                WIN_H = (int)nh;
                have_mouse = 0;
                last_buttons = 0;
                update = 1;
                continue;
            }
        }
        
        if (update) {
            render_editor();
            render_ui();
            if (comp_send_commit(&conn, surface_id, 32, 32, 0u) != 0) {
                ed.quit = 1;
            }
        }
        comp_wait_events(&conn, 4000u);
    }

    (void)comp_send_destroy_surface(&conn, surface_id, 0u);
    if (canvas && size_bytes) {
        munmap((void*)canvas, size_bytes);
        canvas = 0;
    }
    if (shm_fd >= 0) {
        close(shm_fd);
        shm_fd = -1;
    }
    if (shm_name[0]) {
        shm_unlink_named(shm_name);
        shm_name[0] = '\0';
    }
    comp_disconnect(&conn);
    
    lines_destroy(&ed.lines);
    gb_destroy(&ed.text);

    ustack_destroy(&ed.undo);
    ustack_destroy(&ed.redo);
    return 0;
}