// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include <yula.h>

#define EDITOR_BUF_SIZE 16384
#define VIEW_HEIGHT     7

#define C_BG            0x1E1E1E
#define C_FG_DEFAULT    0xD4D4D4
#define C_FG_LINE_NUM   0x858585
#define C_SEL_BG        0x264F78
#define C_SEL_FG        0xFFFFFF
#define C_CURSOR        0xAEAFAD
#define C_DEFAULT_FG    0xD4D4D4
#define C_DEFAULT_BG    0x141414

#define C_SYN_KEYWORD   0x569CD6 
#define C_SYN_DIRECTIVE 0xC586C0 
#define C_SYN_NUMBER    0xB5CEA8 
#define C_SYN_STRING    0xCE9178 
#define C_SYN_COMMENT   0x6A9955 

enum KeyCodes {
    KEY_TAB         = 0x09,
    KEY_ENTER       = 0x0A,
    KEY_BACKSPACE   = 0x08,
    KEY_ESC         = 0x1B,
    
    KEY_CTRL_C      = 0x03,
    KEY_CTRL_Q      = 0x17,
    KEY_CTRL_S      = 0x15,
    KEY_CTRL_V      = 0x16,
    
    KEY_UP          = 0x13,
    KEY_DOWN        = 0x14,
    KEY_LEFT        = 0x11,
    KEY_RIGHT       = 0x12,
    
    KEY_S_UP        = 0x80,
    KEY_S_DOWN      = 0x81,
    KEY_S_LEFT      = 0x82,
    KEY_S_RIGHT     = 0x83,
    
    KEY_C_LEFT      = 0x84,
    KEY_C_RIGHT     = 0x85,
    
    KEY_SC_LEFT     = 0x86,
    KEY_SC_RIGHT    = 0x87
};


typedef struct {
    char* data;             
    int   capacity;         
    int   length;           
    int   cursor;           
    int   sel_start;        
    int   scroll_offset;    
    char  filename[64];     
    int   is_asm;           
    int   dirty;            
    int   should_exit;      
} Editor;


const char* asm_keywords[] = {
    "mov", "int", "push", "pop", "ret", "call", "jmp", 
    "add", "sub", "xor", "or", "and", "cmp", "test",
    "je", "jne", "jg", "jl", "jge", "jle", "jz", "jnz", 
    "inc", "dec", "mul", "div", "hlt", "cli", "sti", 
    "nop", "lea", "loop", 0
};

const char* asm_directives[] = {
    "section", "global", "extern", "public",
    "db", "dw", "dd", "dq", "rb", "rw", "rd", "resb",
    "use32", "use16", "use64", "format", "org", "entry",
    "eax", "ebx", "ecx", "edx", "esi", "edi", "esp", "ebp",
    "ax", "bx", "cx", "dx", "al", "ah", "bl", "bh",
    0
};


int is_word_char(char c) {
    return ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_');
}

int is_delimiter(char c) {
    return (c == ' ' || c == '\n' || c == '\t' || c == ',' || c == '[' || c == ']' || 
            c == '+' || c == '-' || c == '*' || c == '/' || c == ':' || c == 0);
}

int max(int a, int b) { return a > b ? a : b; }
int min(int a, int b) { return a < b ? a : b; }

void editor_insert_char(Editor* ed, char c) {
    if (ed->length >= ed->capacity - 1) return; 

    for (int i = ed->length; i > ed->cursor; i--) {
        ed->data[i] = ed->data[i-1];
    }
    
    ed->data[ed->cursor] = c;
    ed->length++;
    ed->cursor++;
    ed->data[ed->length] = '\0';
    ed->dirty = 1;
}

void editor_insert_string(Editor* ed, const char* str, int len) {
    if (ed->length + len >= ed->capacity - 1) return;

    for (int i = ed->length; i >= ed->cursor; i--) {
        ed->data[i + len] = ed->data[i];
    }

    for (int i = 0; i < len; i++) {
        ed->data[ed->cursor + i] = str[i];
    }

    ed->length += len;
    ed->cursor += len;
    ed->dirty = 1;
}

void editor_delete_range(Editor* ed, int start, int end) {
    if (start >= end) return;
    int len = end - start;

    for (int i = start; i < ed->length - len; i++) {
        ed->data[i] = ed->data[i + len];
    }
    
    ed->length -= len;
    ed->data[ed->length] = '\0';
    ed->cursor = start;
    ed->sel_start = -1;
    ed->dirty = 1;
}

void editor_delete_selection(Editor* ed) {
    if (ed->sel_start != -1 && ed->sel_start != ed->cursor) {
        int start = min(ed->sel_start, ed->cursor);
        int end   = max(ed->sel_start, ed->cursor);
        editor_delete_range(ed, start, end);
    }
}

void editor_backspace(Editor* ed) {
    if (ed->sel_start != -1) {
        editor_delete_selection(ed);
    } else if (ed->cursor > 0) {
        editor_delete_range(ed, ed->cursor - 1, ed->cursor);
    }
}

int get_line_start(Editor* ed, int pos) {
    while (pos > 0 && ed->data[pos - 1] != '\n') pos--;
    return pos;
}

int get_line_length(Editor* ed, int start) {
    int len = 0;
    while (start + len < ed->length && ed->data[start + len] != '\n') len++;
    return len;
}

void move_vertical(Editor* ed, int direction) {
    int curr_line_start = get_line_start(ed, ed->cursor);
    int col = ed->cursor - curr_line_start;
    
    if (direction == -1) {
        if (curr_line_start == 0) return;
        int prev_line_start = get_line_start(ed, curr_line_start - 1);
        int prev_line_len = get_line_length(ed, prev_line_start);
        ed->cursor = (col > prev_line_len) ? (prev_line_start + prev_line_len) : (prev_line_start + col);
    } else {
        int next_line_start = curr_line_start + get_line_length(ed, curr_line_start);
        if (next_line_start < ed->length && ed->data[next_line_start] == '\n') next_line_start++;
        else return;
        if (next_line_start > ed->length) return;
        int next_line_len = get_line_length(ed, next_line_start);
        ed->cursor = (col > next_line_len) ? (next_line_start + next_line_len) : (next_line_start + col);
    }
}

void move_word(Editor* ed, int direction) {
    if (direction == -1) {
        if (ed->cursor > 0) {
            ed->cursor--;
            while (ed->cursor > 0 && !is_word_char(ed->data[ed->cursor])) ed->cursor--;
            while (ed->cursor > 0 && is_word_char(ed->data[ed->cursor - 1])) ed->cursor--;
        }
    } else {
        if (ed->cursor < ed->length) {
            while (ed->cursor < ed->length && is_word_char(ed->data[ed->cursor])) ed->cursor++;
            while (ed->cursor < ed->length && !is_word_char(ed->data[ed->cursor])) ed->cursor++;
        }
    }
}

void editor_load_file(Editor* ed) {
    int fd = open(ed->filename, 0);
    if (fd >= 0) {
        int n = read(fd, ed->data, ed->capacity - 1);
        ed->length = (n > 0) ? n : 0;
        ed->data[ed->length] = 0;
        close(fd);
    } else {
        ed->length = 0;
        ed->data[0] = 0;
    }
    
    int len = strlen(ed->filename);
    if (len > 4 && strcmp(ed->filename + len - 4, ".asm") == 0) ed->is_asm = 1;
    else ed->is_asm = 0;
}

void editor_save_file(Editor* ed) {
    int fd = open(ed->filename, 1);
    if (fd >= 0) {
        write(fd, ed->data, ed->length);
        close(fd);
    }
}

void render_status_bar(Editor* ed) {
    int current_line = 1;
    for (int i = 0; i < ed->cursor; i++) {
        if (ed->data[i] == '\n') current_line++;
    }

    if (ed->sel_start != -1) {
        int len = max(ed->cursor, ed->sel_start) - min(ed->cursor, ed->sel_start);
        printf("SELECTED: %d bytes | %s", len, ed->is_asm ? "ASM" : "TXT");
    } else {
        printf("Line: %d | Size: %d | %s", current_line, ed->length, ed->is_asm ? "ASM" : "TXT"); 
    }
    print("\n");
    print("--------------------------------------------------------------------------------\n");
}

void render_text_area(Editor* ed) {
    int cursor_row = 0;
    for (int i = 0; i < ed->cursor; i++) if (ed->data[i] == '\n') cursor_row++;
    
    if (cursor_row < ed->scroll_offset) ed->scroll_offset = cursor_row;
    if (cursor_row >= ed->scroll_offset + VIEW_HEIGHT) ed->scroll_offset = cursor_row - VIEW_HEIGHT + 1;

    int current_row = 0;
    int syntax_len = 0;
    uint32_t syntax_color = C_FG_DEFAULT;

    int s_min = -1, s_max = -1;
    if (ed->sel_start != -1) {
        s_min = min(ed->sel_start, ed->cursor);
        s_max = max(ed->sel_start, ed->cursor);
    }

    for (int i = 0; i <= ed->length; i++) {
        if (i == ed->cursor) {
            if (current_row >= ed->scroll_offset && current_row < ed->scroll_offset + VIEW_HEIGHT) {
                set_console_color(C_SEL_FG, C_BG);
                char c = (ed->sel_start != -1) ? '#' : '|'; 
                char tmp[2] = {c, 0};
                print(tmp);
            }
        }

        if (i < ed->length) {
            char c = ed->data[i];
            
            if (ed->is_asm && syntax_len == 0) {
                if (c == ';') {
                    syntax_color = C_SYN_COMMENT;
                    int j = i; while (j < ed->length && ed->data[j] != '\n') j++;
                    syntax_len = j - i;
                }
                else if (c == '"' || c == '\'') {
                    syntax_color = C_SYN_STRING;
                    int j = i + 1;
                    while (j < ed->length && ed->data[j] != c && ed->data[j] != '\n') j++;
                    if (j < ed->length && ed->data[j] == c) j++;
                    syntax_len = j - i;
                }
                else if (c >= '0' && c <= '9') {
                    syntax_color = C_SYN_NUMBER;
                    int j = i;
                    while (j < ed->length && !is_delimiter(ed->data[j])) j++;
                    syntax_len = j - i;
                }
                else {
                    int is_start = (i == 0 || is_delimiter(ed->data[i-1]));
                    if (is_start) {
                        for (int k = 0; asm_keywords[k]; k++) {
                            int klen = strlen(asm_keywords[k]);
                            if (strncmp(&ed->data[i], asm_keywords[k], klen) == 0 && is_delimiter(ed->data[i + klen])) {
                                syntax_color = C_SYN_KEYWORD; syntax_len = klen; break;
                            }
                        }
                        if (syntax_len == 0) {
                            for (int k = 0; asm_directives[k]; k++) {
                                int klen = strlen(asm_directives[k]);
                                if (strncmp(&ed->data[i], asm_directives[k], klen) == 0 && is_delimiter(ed->data[i + klen])) {
                                    syntax_color = C_SYN_DIRECTIVE; syntax_len = klen; break;
                                }
                            }
                        }
                    }
                }
            }

            if (current_row >= ed->scroll_offset && current_row < ed->scroll_offset + VIEW_HEIGHT) {
                int is_selected = (ed->sel_start != -1 && i >= s_min && i < s_max);
                
                if (is_selected) {
                    set_console_color(C_SEL_FG, C_SEL_BG);
                } else {
                    if (syntax_len > 0) set_console_color(syntax_color, C_BG);
                    else set_console_color(C_FG_DEFAULT, C_BG);
                }
                
                char tmp[2] = {c, 0};
                print(tmp);
            }
            
            if (c == '\n') {
                current_row++;
                syntax_len = 0; syntax_color = C_FG_DEFAULT;
            } else {
                if (syntax_len > 0) syntax_len--;
            }
        }
        
        if (current_row > ed->scroll_offset + VIEW_HEIGHT) break;
    }
    
    set_console_color(C_FG_DEFAULT, C_BG);
}

void editor_render(Editor* ed) {
    if (!ed->dirty) return;
    
    set_console_color(C_FG_DEFAULT, C_BG);
    
    char cmd = 0x0C; write(1, &cmd, 1);
    
    printf("EDIT: %s  |  [Ctrl+S] Save  [Ctrl+Q] Quit\n", ed->filename);
    render_status_bar(ed);
    render_text_area(ed);
    
    ed->dirty = 0; 
}

void handle_input(Editor* ed, char c) {
    unsigned char uc = (unsigned char)c;
    ed->dirty = 1;

    int is_shift = (uc >= KEY_S_UP && uc <= KEY_S_RIGHT) || uc == KEY_SC_LEFT || uc == KEY_SC_RIGHT;
    int is_nav   = (uc >= KEY_LEFT && uc <= KEY_DOWN) || uc == KEY_C_LEFT || uc == KEY_C_RIGHT;

    if (is_shift) {
        if (ed->sel_start == -1) ed->sel_start = ed->cursor;
    } else if (is_nav) {
        ed->sel_start = -1; 
    }

    switch (uc) {
        case KEY_LEFT:    if (ed->cursor > 0) ed->cursor--; break;
        case KEY_RIGHT:   if (ed->cursor < ed->length) ed->cursor++; break;
        case KEY_UP:      move_vertical(ed, -1); break;
        case KEY_DOWN:    move_vertical(ed, 1); break;
        
        case KEY_C_LEFT:  move_word(ed, -1); break;
        case KEY_C_RIGHT: move_word(ed, 1); break;

        case KEY_S_LEFT:  if (ed->cursor > 0) ed->cursor--; break;
        case KEY_S_RIGHT: if (ed->cursor < ed->length) ed->cursor++; break;
        case KEY_S_UP:    move_vertical(ed, -1); break;
        case KEY_S_DOWN:  move_vertical(ed, 1); break;
        case KEY_SC_LEFT: move_word(ed, -1); break;
        case KEY_SC_RIGHT:move_word(ed, 1); break;

        case KEY_CTRL_C: 
            if (ed->sel_start != -1 && ed->sel_start != ed->cursor) {
                int start = min(ed->sel_start, ed->cursor);
                int len = max(ed->sel_start, ed->cursor) - start;
                char* tmp = malloc(len + 1);
                if (tmp) {
                    for(int i=0; i<len; i++) tmp[i] = ed->data[start+i];
                    tmp[len] = 0;
                    clipboard_copy(tmp);
                    free(tmp);
                }
            } else {
                clipboard_copy(ed->data);
            }
            break;

        case KEY_CTRL_V: {
            editor_delete_selection(ed);
            char* tmp = malloc(4096);
            if (tmp) {
                int len = clipboard_paste(tmp, 4096);
                if (len > 0) editor_insert_string(ed, tmp, len);
                free(tmp);
            }
            break;
        }

        case KEY_TAB:
            editor_delete_selection(ed);
            editor_insert_string(ed, "    ", 4);
            break;

        case KEY_BACKSPACE:
            editor_backspace(ed);
            break;

        case KEY_CTRL_S:
            editor_save_file(ed);
            break;

        case KEY_CTRL_Q:
            ed->should_exit = 1;
            break;

        case KEY_ENTER:
            editor_delete_selection(ed);
            editor_insert_char(ed, '\n');
            break;

        default:
            if (ed->length < ed->capacity - 1 && (uc >= 32 || c == '\n')) {
                editor_delete_selection(ed);
                editor_insert_char(ed, c);
            }
            break;
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        print("Usage: edit <filename>\n");
        return 1;
    }

    Editor ed;
    memset(&ed, 0, sizeof(Editor));
    strcpy(ed.filename, argv[1]);
    ed.capacity = EDITOR_BUF_SIZE;
    ed.data = malloc(EDITOR_BUF_SIZE);
    ed.sel_start = -1;
    ed.dirty = 1;
    ed.should_exit = 0;

    if (!ed.data) {
        print("Error: Out of memory\n");
        return 1;
    }
    
    for(int i=0; i<EDITOR_BUF_SIZE; i++) ed.data[i] = 0;

    set_term_mode(1);
    editor_load_file(&ed);
    
    int kbd_fd = open("/dev/kbd", 0);

    while (!ed.should_exit) {
        editor_render(&ed);
        
        char c;
        int n = read(kbd_fd, &c, 1);
        if (n > 0) {
            handle_input(&ed, c);
        }
    }

    close(kbd_fd);
    free(ed.data);
    
    set_console_color(C_DEFAULT_FG, C_DEFAULT_BG); 
    char cls = 0x0C; write(1, &cls, 1);
    
    return 0;
}