#include "flux_cursor.h"

static const char* k_cursor_arrow[] = {
    "X           ",
    "XX          ",
    "X.X         ",
    "X..X        ",
    "X...X       ",
    "X....X      ",
    "X.....X     ",
    "X......X    ",
    "X.......X   ",
    "X........X  ",
    "X.....XXXXX ",
    "X..X..X     ",
    "X.X X..X    ",
    "XX  X..X    ",
    "X    X..X   ",
    "     XXXX   ",
    0
};

int flux_cursor_draw_arrow(void* ctx, int x, int y, flux_cursor_draw_rect_fn draw_fn) {
    if (!draw_fn) return -1;

    for (int r = 0; k_cursor_arrow[r]; r++) {
        const char* row_str = k_cursor_arrow[r];
        int c = 0;
        while (row_str[c]) {
            char type = row_str[c];
            if (type == ' ') {
                c++;
                continue;
            }

            int start = c;
            int len = 0;
            while (row_str[c] == type) {
                len++;
                c++;
            }

            int color_type = (type == 'X') ? 0 : 1;

            if (draw_fn(ctx, x + start, y + r, len, 1, color_type) != 0) {
                return -1;
            }
        }
    }

    return 0;
}
