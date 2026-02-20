#include <kernel/tty/tty.h>

#include <kernel/proc.h>

#include <drivers/vga.h>
#include <drivers/fbdev.h>

#include <kernel/term/term.h>
#include <kernel/tty/tty_service.h>
#include <kernel/tty/tty_internal.h>


static kernel::term::TermSnapshot tty_snapshot;
static kernel::term::VgaTermRenderer tty_renderer;

static void tty_render_fallback(void) {
    vga_set_target(0, 0, 0);
    vga_draw_rect(0, 0, (int)fb_width, (int)fb_height, 0x000000);
    vga_print_at("TTY: waiting for shell...", 16, 16, COLOR_LIGHT_GREY);
    vga_mark_dirty(0, 0, (int)fb_width, (int)fb_height);
    vga_flip_dirty();
    vga_reset_dirty();
}

extern "C" void tty_task(void* arg) {
    (void)arg;

    kernel::tty::TtyService::instance().request_render(kernel::tty::TtyService::RenderReason::Output);

    uint64_t last_seq = 0;
    uint64_t last_view_seq = 0;

    int last_cursor_row = -1;
    int last_cursor_col = -1;

    int fb_was_renderable = 0;

    for (;;) {
        kernel::tty::TtyService::instance().render_wait();

        uint32_t reasons = kernel::tty::TtyService::instance().consume_render_requests();

        if ((reasons & static_cast<uint32_t>(kernel::tty::TtyService::RenderReason::ActiveChanged)) != 0u
            || (reasons & static_cast<uint32_t>(kernel::tty::TtyService::RenderReason::Resize)) != 0u) {
            last_seq = (uint64_t)-1;
            last_view_seq = (uint64_t)-1;
            last_cursor_row = -1;
            last_cursor_col = -1;
        }

        while (kernel::tty::TtyService::instance().render_try_acquire()) {
        }

        int fb_renderable = fb_kernel_can_render() ? 1 : 0;
        if (!fb_renderable) {
            fb_was_renderable = 0;
            proc_usleep(10000);
            kernel::tty::TtyService::instance().request_render(kernel::tty::TtyService::RenderReason::Output);
            continue;
        }

        if (!fb_was_renderable) {
            fb_was_renderable = 1;
            last_seq = (uint64_t)-1;
            last_view_seq = (uint64_t)-1;
            last_cursor_row = -1;
            last_cursor_col = -1;
        }

        tty_handle_t* tty = kernel::tty::TtyService::instance().get_active_for_render();
        kernel::term::Term* term = tty_term_ptr(tty);

        if (!term) {
            tty_render_fallback();
            continue;
        }

        uint64_t cur_seq = term->seq();
        uint64_t cur_view_seq = term->view_seq();

        if (cur_seq == last_seq && cur_view_seq == last_view_seq) {
            continue;
        }

        int ok = term->capture_snapshot(tty_snapshot);
        if (ok != 0) {
            continue;
        }

        uint32_t bg = tty_snapshot.curr_bg();
        int full_redraw = tty_snapshot.full_redraw();

        int cols = tty_snapshot.cols();
        int view_rows = tty_snapshot.view_rows();

        int cur_row = tty_snapshot.cursor_row();
        int cur_col = tty_snapshot.cursor_col();

        if (cur_row != last_cursor_row || cur_col != last_cursor_col) {
            if (last_cursor_row >= 0 && last_cursor_row < view_rows) {
                tty_snapshot.mark_dirty_cell(last_cursor_row, last_cursor_col);
                (void)term->capture_cell(tty_snapshot, last_cursor_row, last_cursor_col);
            }

            if (cur_row >= 0 && cur_row < view_rows) {
                tty_snapshot.mark_dirty_cell(cur_row, cur_col);
                (void)term->capture_cell(tty_snapshot, cur_row, cur_col);
            }
        }

        vga_set_target(0, 0, 0);

        int term_x = 0;
        int term_y = 0;

        int term_w = cols * 8;
        int term_h = view_rows * 16;

        if (term_w > (int)fb_width) {
            term_w = (int)fb_width;
        }

        if (term_h > (int)fb_height) {
            term_h = (int)fb_height;
        }

        int bb_x1 = 0;
        int bb_y1 = 0;
        int bb_x2 = 0;
        int bb_y2 = 0;
        int have_bbox = (tty_snapshot.dirty_bbox(bb_x1, bb_y1, bb_x2, bb_y2) == 0) ? 1 : 0;

        if (full_redraw) {
            vga_draw_rect(term_x, term_y, term_w, term_h, bg);
            tty_renderer.render(tty_snapshot, term_x, term_y);
            vga_mark_dirty(term_x, term_y, term_w, term_h);
        } else if (have_bbox) {
            tty_renderer.render(tty_snapshot, term_x, term_y);
            vga_mark_dirty(term_x + bb_x1 * 8, term_y + bb_y1 * 16, (bb_x2 - bb_x1) * 8, (bb_y2 - bb_y1) * 16);
        }

        int rel_row = tty_snapshot.cursor_row();
        if (rel_row >= 0 && rel_row < view_rows) {
            int cx = tty_snapshot.cursor_col();

            if (cx < 0) {
                cx = 0;
            }

            if (cx >= cols) {
                cx = cols - 1;
            }

            vga_draw_rect(term_x + cx * 8, term_y + rel_row * 16 + 14, 8, 2, COLOR_LIGHT_GREEN);
            vga_mark_dirty(term_x + cx * 8, term_y + rel_row * 16 + 14, 8, 2);
        }

        vga_flip_dirty();
        vga_reset_dirty();

        last_seq = cur_seq;
        last_view_seq = cur_view_seq;
        last_cursor_row = cur_row;
        last_cursor_col = cur_col;
    }
}
