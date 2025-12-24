#include <yula.h>
#include <font.h>

#define MAX_TEXT 8192
#define WIN_W 500
#define WIN_H 400
#define PADDING_TOP 30
#define PADDING_LEFT 10
#define LINE_HEIGHT 12
#define CHAR_WIDTH 8

char text_buf[MAX_TEXT];
int text_len = 0;
int cursor = 0;
char filename[64];
int show_status = 0;

int get_line_start(int pos) {
    while (pos > 0 && text_buf[pos - 1] != '\n') pos--;
    return pos;
}

int get_line_length(int start) {
    int len = 0;
    while (start + len < text_len && text_buf[start + len] != '\n') len++;
    return len;
}

void move_up() {
    int curr_start = get_line_start(cursor);
    if (curr_start == 0) return;
    int col = cursor - curr_start;
    int prev_start = get_line_start(curr_start - 1);
    int prev_len = get_line_length(prev_start);
    cursor = (col > prev_len) ? (prev_start + prev_len) : (prev_start + col);
}

void move_down() {
    int curr_start = get_line_start(cursor);
    int col = cursor - curr_start;
    int next_start = curr_start + get_line_length(curr_start);
    if (next_start < text_len && text_buf[next_start] == '\n') next_start++;
    else return;
    if (next_start > text_len) return;
    int next_len = get_line_length(next_start);
    cursor = (col > next_len) ? (next_start + next_len) : (next_start + col);
}

void insert_char(char c) {
    if (text_len >= MAX_TEXT - 1) return;
    for (int i = text_len; i > cursor; i--) text_buf[i] = text_buf[i-1];
    text_buf[cursor] = c;
    text_len++;
    cursor++;
    text_buf[text_len] = 0;
}

void delete_char() {
    if (cursor > 0) {
        for (int i = cursor; i < text_len; i++) text_buf[i-1] = text_buf[i];
        text_len--;
        cursor--;
        text_buf[text_len] = 0;
    }
}

void render(uint32_t* buf, int w, int h) {
    for (int i = 0; i < w * h; i++) buf[i] = 0x1E1E1E;

    for (int y = 0; y < 24; y++) 
        for (int x = 0; x < w; x++) buf[y * w + x] = 0x333333;
    
    if (show_status > 0) {
        draw_string(buf, w, h, 10, 8, "FILE SAVED!", 0x00FF00);
        show_status--;
    } else {
        char title[128];
        draw_string(buf, w, h, 10, 8, "File: ", 0xAAAAAA);
        draw_string(buf, w, h, 60, 8, filename, 0xFFFFFF);
        draw_string(buf, w, h, 260, 8, "[Ctrl+S] Save  [Ctrl+Q] Quit", 0x888888);
    }

    int cx = PADDING_LEFT;
    int cy = PADDING_TOP;
    
    int cursor_px_x = -1, cursor_px_y = -1;

    for (int i = 0; i <= text_len; i++) {
        if (i == cursor) {
            cursor_px_x = cx;
            cursor_px_y = cy;
        }
        
        if (i == text_len) break;

        char c = text_buf[i];
        
        if (c == '\n') {
            cx = PADDING_LEFT;
            cy += LINE_HEIGHT;
        } else {
            if (cy < h - 10) {
                draw_char(buf, w, h, cx, cy, c, 0xD4D4D4);
            }
            cx += CHAR_WIDTH;
        }
    }

    if (cursor_px_x != -1) {
        for (int dy = 0; dy < 10; dy++) {
            for (int dx = 0; dx < 2; dx++) {
                int px = cursor_px_x + dx;
                int py = cursor_px_y + dy;
                if (px < w && py < h) buf[py * w + px] = 0x00FF00;
            }
        }
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        strcpy(filename, "untitled.txt");
    } else {
        strcpy(filename, argv[1]);
        int fd = open(filename, 0);
        if (fd >= 0) {
            int n = read(fd, text_buf, MAX_TEXT - 1);
            text_len = (n > 0) ? n : 0;
            text_buf[text_len] = 0;
            close(fd);
        }
    }

    int win_id = create_window(WIN_W, WIN_H, "Graphic Editor");
    if (win_id < 0) return 1;
    
    uint32_t* buffer = (uint32_t*)map_window(win_id);
    if (!buffer) return 1;
    
    render(buffer, WIN_W, WIN_H);
    update_window(win_id);
    
    while (1) {
        yula_event_t ev;
        int need_repaint = 0;
        
        while (get_event(win_id, &ev)) {
            if (ev.type == YULA_EVENT_KEY_DOWN) {
                char c = (char)ev.arg1;
                
                if (c == 0x11) { // Left
                    if (cursor > 0) cursor--;
                }
                else if (c == 0x12) { // Right
                    if (cursor < text_len) cursor++;
                }
                else if (c == 0x13) move_up();   // Up
                else if (c == 0x14) move_down(); // Down
                
                else if (c == 0x17) { // Ctrl+Q
                    return 0; 
                }
                else if (c == 0x15) { // Ctrl+S
                    int f_out = open(filename, 1);
                    if (f_out >= 0) {
                        write(f_out, text_buf, text_len);
                        close(f_out);
                        show_status = 50;
                    }
                }
                else if (c == '\b') {
                    delete_char();
                }
                else if (c == '\n' || (c >= 32 && c <= 126)) {
                    insert_char(c);
                }
                
                need_repaint = 1;
            }
        }
        
        if (show_status > 0) {
            need_repaint = 1; 
        }

        if (need_repaint) {
            render(buffer, WIN_W, WIN_H);
            update_window(win_id);
        }
        
        usleep(10000);
    }
    return 0;
}