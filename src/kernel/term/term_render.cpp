#include <kernel/term/term.h>

#include <drivers/vga.h>

namespace kernel::term {

void VgaTermRenderer::render(const TermSnapshot& snapshot, int win_x, int win_y) const {
    int cols = snapshot.cols();
    int view_rows = snapshot.view_rows();

    if (cols < 1 || view_rows < 1) {
        return;
    }

    for (int y = 0; y < view_rows; y++) {
        int x0 = 0;
        int x1 = cols;

        if (snapshot.full_redraw() == 0) {
            if (snapshot.dirty_row_range(y, x0, x1) != 0) {
                continue;
            }
        }

        if (x0 < 0) {
            x0 = 0;
        }

        if (x1 > cols) {
            x1 = cols;
        }

        if (x0 >= x1) {
            continue;
        }

        int py = win_y + y * 16;

        int run_x0 = x0;
        uint32_t run_bg = snapshot.bg_at(y, x0);

        for (int x = x0 + 1; x < x1; x++) {
            uint32_t bg = snapshot.bg_at(y, x);
            if (bg == run_bg) {
                continue;
            }

            int px = win_x + run_x0 * 8;
            vga_draw_rect(px, py, (x - run_x0) * 8, 16, run_bg);

            run_x0 = x;
            run_bg = bg;
        }

        {
            int px = win_x + run_x0 * 8;
            vga_draw_rect(px, py, (x1 - run_x0) * 8, 16, run_bg);
        }

        for (int x = x0; x < x1; x++) {
            char ch = snapshot.ch_at(y, x);
            if (ch == ' ') {
                continue;
            }

            int px = win_x + x * 8;
            vga_draw_char_sse(px, py, ch, snapshot.fg_at(y, x));
        }
    }
}

}
