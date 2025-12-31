// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include <yula.h>
#include <font.h>

#define WIN_W 800
#define WIN_H 600

#define C_BG            0x1E1E1E
#define C_GUTTER_BG     0x1E1E1E 
#define C_GUTTER_FG     0x858585
#define C_ACTIVE_LINE   0x282828 
#define C_SELECTION     0x264F78 
#define C_STATUS_BG     0x007ACC 
#define C_STATUS_FG     0xFFFFFF
#define C_TAB_BG        0x252526
#define C_TAB_FG        0xCCCCCC
#define C_TEXT          0xD4D4D4
#define C_CURSOR        0xFFFFFF

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

#define MAX_TEXT    65536

typedef struct {
    char* data;
    int   capacity;
    int   length;
    int   cursor;
    int   sel_bound; 
    int   scroll_y;
    char  filename[64];
    int   dirty;
    int   quit;
    int   pref_col;   
    int   is_dragging;
} Editor;

uint32_t* canvas;
int win_id;
Editor ed;

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

int min(int a, int b) { return a < b ? a : b; }
int max(int a, int b) { return a > b ? a : b; }

int is_digit(char c) { return (c >= '0' && c <= '9'); }
int is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '.'; }
int is_word_char(char c) { return is_alpha(c) || is_digit(c); }

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
    while (pos > 0 && ed.data[pos - 1] != '\n') pos--;
    return pos;
}

int get_line_len(int start) {
    int len = 0;
    while (start + len < ed.length && ed.data[start + len] != '\n') len++;
    return len;
}

void update_pref_col() {
    ed.pref_col = ed.cursor - get_line_start(ed.cursor);
}

void delete_range(int start, int end) {
    if (start >= end) return;
    int len = end - start;
    for (int i = start; i < ed.length - len; i++) {
        ed.data[i] = ed.data[i + len];
    }
    ed.length -= len;
    ed.data[ed.length] = 0;
    
    ed.cursor = start;
    ed.sel_bound = -1;
    ed.dirty = 1;
    update_pref_col();
}

void insert_str(const char* s, int len) {
    if (ed.sel_bound != -1) {
        int mn = min(ed.sel_bound, ed.cursor);
        int mx = max(ed.sel_bound, ed.cursor);
        delete_range(mn, mx);
    }
    
    if (ed.length + len >= ed.capacity) return;
    
    for (int i = ed.length; i >= ed.cursor; i--) {
        ed.data[i + len] = ed.data[i];
    }
    
    for (int i = 0; i < len; i++) {
        ed.data[ed.cursor + i] = s[i];
    }
    
    ed.length += len;
    ed.cursor += len;
    ed.dirty = 1;
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
    int len = e - s;
    
    char* tmp = malloc(len + 1);
    if (!tmp) return;
    
    for (int i = 0; i < len; i++) {
        tmp[i] = ed.data[s + i];
    }
    tmp[len] = 0;
    
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
    if (ed.cursor < ed.length) ed.cursor++;
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
    if (next_start < ed.length) {
        int next_len = get_line_len(next_start);
        int target = min(next_len, ed.pref_col);
        ed.cursor = next_start + target;
    } else {
        ed.cursor = ed.length;
    }
}

void move_word_left(int select) {
    handle_selection(select);
    if (ed.cursor == 0) return;
    ed.cursor--;
    while (ed.cursor > 0 && !is_word_char(ed.data[ed.cursor])) ed.cursor--;
    while (ed.cursor > 0 && is_word_char(ed.data[ed.cursor-1])) ed.cursor--;
    update_pref_col();
}

void move_word_right(int select) {
    handle_selection(select);
    if (ed.cursor >= ed.length) return;
    while (ed.cursor < ed.length && is_word_char(ed.data[ed.cursor])) ed.cursor++;
    while (ed.cursor < ed.length && !is_word_char(ed.data[ed.cursor])) ed.cursor++;
    update_pref_col();
}

void load_file() {
    int fd = open(ed.filename, 0);
    if (fd >= 0) {
        int r = read(fd, ed.data, ed.capacity - 1);
        ed.length = (r > 0) ? r : 0;
        ed.data[ed.length] = 0;
        close(fd);
    }
}

void save_file() {
    int fd = open(ed.filename, 1);
    if (fd >= 0) {
        write(fd, ed.data, ed.length);
        close(fd);
        ed.dirty = 0;
    }
}

int get_pos_from_coords(int mx, int my) {
    int row = (my - TAB_H) / LINE_H;
    int target_line = ed.scroll_y + row;
    
    int curr_l = 0;
    int i = 0;
    
    while(curr_l < target_line && i < ed.length) {
        if (ed.data[i] == '\n') curr_l++;
        i++;
    }
    
    if (curr_l == target_line) {
        int click_x = mx - (GUTTER_W + PAD_X);
        int col = (click_x + (CHAR_W/2)) / CHAR_W;
        
        int len = get_line_len(i);
        if (col < 0) col = 0;
        if (col > len) col = len;
        
        return i + col;
    }
    
    return ed.length;
}


int check_kw(const char* text, int len, const char** list) {
    for(int i=0; list[i]; i++) {
        int l = strlen(list[i]);
        if (l == len && strncmp(text, list[i], len) == 0) return 1;
    }
    return 0;
}

void render_editor() {
    int view_lines = (WIN_H - STATUS_H - TAB_H) / LINE_H;
    int cur_line = 0;
    for(int i=0; i<ed.cursor; i++) if(ed.data[i]=='\n') cur_line++;
    
    if (cur_line < ed.scroll_y) ed.scroll_y = cur_line;
    if (cur_line >= ed.scroll_y + view_lines) ed.scroll_y = cur_line - view_lines + 1;

    draw_rect(0, TAB_H, WIN_W, WIN_H - STATUS_H - TAB_H, C_BG);
    draw_rect(0, TAB_H, GUTTER_W, WIN_H - STATUS_H - TAB_H, C_GUTTER_BG);

    int line_idx = ed.scroll_y;
    int char_idx = 0;
    int lines_skipped = 0;
    while(lines_skipped < ed.scroll_y && char_idx < ed.length) {
        if (ed.data[char_idx] == '\n') lines_skipped++;
        char_idx++;
    }

    int draw_y = TAB_H + 2;
    int s_min = -1, s_max = -1;
    if (ed.sel_bound != -1) {
        s_min = min(ed.sel_bound, ed.cursor);
        s_max = max(ed.sel_bound, ed.cursor);
    }

    while (char_idx <= ed.length && draw_y < WIN_H - STATUS_H) {
        int line_start = char_idx;
        int line_len = get_line_len(line_start);
        int is_active = (cur_line == line_idx);

        if (is_active) draw_rect(GUTTER_W, draw_y, WIN_W - GUTTER_W, LINE_H, C_ACTIVE_LINE);

        char num[12]; fmt_int(line_idx + 1, num);
        int num_w = strlen(num)*8;
        render_string(GUTTER_W - 8 - num_w, draw_y, num, is_active ? C_GUTTER_FG : 0x505050);

        int draw_x = GUTTER_W + PAD_X;
        int i = 0;
        
        while (i < line_len) {
            int abs_pos = line_start + i;
            char c = ed.data[abs_pos];
            uint32_t fg = C_TEXT;
            
            int is_sel = (ed.sel_bound != -1 && abs_pos >= s_min && abs_pos < s_max);
            if (is_sel) draw_rect(draw_x, draw_y, CHAR_W, LINE_H, C_SELECTION);
            if (abs_pos == ed.cursor) draw_rect(draw_x, draw_y, 2, LINE_H, C_CURSOR);

            if (c == ';') {
                fg = C_SYN_COMMENT;
                while (i < line_len) {
                    abs_pos = line_start + i;
                    is_sel = (ed.sel_bound != -1 && abs_pos >= s_min && abs_pos < s_max);
                    if (is_sel) draw_rect(draw_x, draw_y, CHAR_W, LINE_H, C_SELECTION);
                    if (abs_pos == ed.cursor) draw_rect(draw_x, draw_y, 2, LINE_H, C_CURSOR);
                    render_char(draw_x, draw_y, ed.data[abs_pos], fg);
                    draw_x += CHAR_W; i++;
                }
                break;
            }
            else if (c == '"' || c == '\'') {
                char q = c;
                fg = C_SYN_STRING;
                render_char(draw_x, draw_y, c, fg);
                draw_x += CHAR_W; i++;
                while (i < line_len) {
                    abs_pos = line_start + i;
                    char cc = ed.data[abs_pos];
                    is_sel = (ed.sel_bound != -1 && abs_pos >= s_min && abs_pos < s_max);
                    if (is_sel) draw_rect(draw_x, draw_y, CHAR_W, LINE_H, C_SELECTION);
                    if (abs_pos == ed.cursor) draw_rect(draw_x, draw_y, 2, LINE_H, C_CURSOR);
                    render_char(draw_x, draw_y, cc, fg);
                    draw_x += CHAR_W; i++;
                    if (cc == q) break;
                }
                continue;
            }
            else if (is_word_char(c) && (i==0 || !is_word_char(ed.data[line_start+i-1]))) {
                int wlen = 0;
                while (i+wlen < line_len && is_word_char(ed.data[line_start+i+wlen])) wlen++;
                
                if (check_kw(&ed.data[abs_pos], wlen, kwd_general)) fg = C_SYN_KEYWORD;
                else if (check_kw(&ed.data[abs_pos], wlen, kwd_control)) fg = C_SYN_CONTROL;
                else if (check_kw(&ed.data[abs_pos], wlen, kwd_dirs)) fg = C_SYN_DIRECTIVE;
                else if (check_kw(&ed.data[abs_pos], wlen, kwd_regs)) fg = C_SYN_REG;
                else if (is_digit(c)) fg = C_SYN_NUMBER;
                
                for (int k=0; k<wlen; k++) {
                    abs_pos = line_start + i;
                    is_sel = (ed.sel_bound != -1 && abs_pos >= s_min && abs_pos < s_max);
                    if (is_sel) draw_rect(draw_x, draw_y, CHAR_W, LINE_H, C_SELECTION);
                    if (abs_pos == ed.cursor) draw_rect(draw_x, draw_y, 2, LINE_H, C_CURSOR);
                    render_char(draw_x, draw_y, ed.data[abs_pos], fg);
                    draw_x += CHAR_W; i++;
                }
                continue;
            }

            render_char(draw_x, draw_y, c, fg);
            draw_x += CHAR_W; i++;
        }

        if (line_start + line_len == ed.cursor) draw_rect(draw_x, draw_y, 2, LINE_H, C_CURSOR);
        char_idx += line_len + 1;
        line_idx++;
        draw_y += LINE_H;
    }
}

void render_ui() {
    draw_rect(0, 0, WIN_W, TAB_H, C_GUTTER_BG);
    draw_rect(0, 0, 150, TAB_H, C_TAB_BG);
    draw_rect(0, TAB_H-1, 150, 1, C_STATUS_BG);
    render_string(10, 8, ed.filename, C_TAB_FG);
    if (ed.dirty) render_string(10 + strlen(ed.filename)*8, 8, "*", C_TAB_FG);

    int y = WIN_H - STATUS_H;
    draw_rect(0, y, WIN_W, STATUS_H, C_STATUS_BG);
    
    int line = 1, col = 1;
    for(int i=0; i<ed.cursor; i++) if(ed.data[i]=='\n') { line++; col=1; } else col++;
    
    char l[8], c[8]; fmt_int(line, l); fmt_int(col, c);
    render_string(WIN_W - 120, y+8, "Ln", C_STATUS_FG);
    render_string(WIN_W - 100, y+8, l, C_STATUS_FG);
    render_string(WIN_W - 60, y+8, "Col", C_STATUS_FG);
    render_string(WIN_W - 30, y+8, c, C_STATUS_FG);
}

int main(int argc, char** argv) {
    if (argc < 2) strcpy(ed.filename, "new.asm");
    else strcpy(ed.filename, argv[1]);

    ed.capacity = MAX_TEXT;
    ed.data = malloc(MAX_TEXT);
    if (!ed.data) return 1;
    memset(ed.data, 0, MAX_TEXT);
    
    ed.sel_bound = -1;
    ed.dirty = 1;
    ed.is_dragging = 0;
    
    load_file();

    win_id = create_window(WIN_W, WIN_H, "GEditor");
    if (win_id < 0) return 1;
    canvas = (uint32_t*)map_window(win_id);

    render_editor();
    render_ui();
    update_window(win_id);

    yula_event_t ev;
    while (!ed.quit) {
        int update = 0;
        while (get_event(win_id, &ev)) {
            if (ev.type == YULA_EVENT_KEY_DOWN) {
                unsigned char c = (unsigned char)ev.arg1;
                
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
                else if (c == 0x0A) insert_char('\n');
                else if (c == 0x09) insert_str("    ", 4);
                else if (c == 0x15) save_file(); // Ctrl+S
                else if (c == 0x17) ed.quit = 1; // Ctrl+Q
                else if (c == 0x03) copy_selection(); // Ctrl+C
                else if (c == 0x16) paste_clipboard(); // Ctrl+V
                
                else if (c >= 32 && c <= 126) insert_char((char)c);
                
                update = 1;
            }
            else if (ev.type == YULA_EVENT_MOUSE_DOWN) {
                int pos = get_pos_from_coords(ev.arg1, ev.arg2);
                ed.cursor = pos;
                ed.sel_bound = pos;
                ed.is_dragging = 1;
                update_pref_col();
                update = 1;
            }
            else if (ev.type == YULA_EVENT_MOUSE_MOVE) {
                if (ed.is_dragging) {
                    int pos = get_pos_from_coords(ev.arg1, ev.arg2);
                    if (pos != ed.cursor) {
                        ed.cursor = pos;
                        update_pref_col();
                        update = 1;
                    }
                }
            }
            else if (ev.type == YULA_EVENT_MOUSE_UP) {
                ed.is_dragging = 0;
                if (ed.sel_bound == ed.cursor) ed.sel_bound = -1;
                update = 1;
            }
        }
        
        if (update) {
            render_editor();
            render_ui();
            update_window(win_id);
        }
        usleep(4000);
    }
    
    free(ed.data);
    return 0;
}