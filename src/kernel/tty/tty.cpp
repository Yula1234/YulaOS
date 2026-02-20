#include <kernel/tty/tty_internal.h>

#include <kernel/tty/tty_service.h>

#include <drivers/fbdev.h>

#include <hal/lock.h>

#include <lib/cpp/new.h>

#include <lib/string.h>
#include <mm/heap.h>

static void tty_default_size(int& out_cols, int& out_view_rows) {
    int cols = (int)(fb_width / 8u);
    int view_rows = (int)(fb_height / 16u);

    if (cols < 1) {
        cols = 1;
    }

    if (view_rows < 1) {
        view_rows = 1;
    }

    out_cols = cols;
    out_view_rows = view_rows;
}

extern "C" tty_handle_t* tty_create_default(void) {
    tty_handle_t* tty = (tty_handle_t*)kmalloc(sizeof(*tty));
    if (!tty) {
        return 0;
    }

    memset(tty, 0, sizeof(*tty));

    int cols = 0;
    int view_rows = 0;
    tty_default_size(cols, view_rows);

    kernel::tty::TtySession* session = kernel::tty::TtySession::create(cols, view_rows);
    if (!session) {
        kfree(tty);
        return 0;
    }

    tty->session = session;

    return tty;
}

extern "C" void tty_destroy(tty_handle_t* tty) {
    if (!tty) {
        return;
    }

    kernel::tty::TtyService::instance().clear_active_if_matches(tty);

    if (tty->session) {
        delete tty->session;
        tty->session = nullptr;
    }

    kfree(tty);
}

extern "C" void tty_set_active(tty_handle_t* tty) {
    kernel::tty::TtyService::instance().set_active(tty);
    kernel::tty::TtyService::instance().request_render(kernel::tty::TtyService::RenderReason::ActiveChanged);
}

extern "C" void tty_write(tty_handle_t* tty, const char* buf, uint32_t len) {
    kernel::term::Term* term = tty_term_ptr(tty);
    if (!term || !buf || len == 0) {
        return;
    }

    term->write(buf, len);

    kernel::tty::TtyService::instance().request_render(kernel::tty::TtyService::RenderReason::Output);
}

extern "C" void tty_print(tty_handle_t* tty, const char* s) {
    kernel::term::Term* term = tty_term_ptr(tty);
    if (!term || !s) {
        return;
    }

    term->print(s);

    kernel::tty::TtyService::instance().request_render(kernel::tty::TtyService::RenderReason::Output);
}

extern "C" void tty_putc(tty_handle_t* tty, char c) {
    kernel::term::Term* term = tty_term_ptr(tty);
    if (!term) {
        return;
    }

    term->putc(c);

    kernel::tty::TtyService::instance().request_render(kernel::tty::TtyService::RenderReason::Output);
}

extern "C" void tty_set_colors(tty_handle_t* tty, uint32_t fg, uint32_t bg) {
    kernel::term::Term* term = tty_term_ptr(tty);
    if (!term) {
        return;
    }

    term->set_colors(fg, bg);

    kernel::tty::TtyService::instance().request_render(kernel::tty::TtyService::RenderReason::Output);
}

extern "C" int tty_get_winsz(tty_handle_t* tty, yos_winsize_t* out_ws) {
    kernel::term::Term* term = tty_term_ptr(tty);
    if (!term || !out_ws) {
        return -1;
    }

    uint16_t cols = 0;
    uint16_t rows = 0;
    if (term->get_winsz(cols, rows) != 0) {
        return -1;
    }

    out_ws->ws_col = cols;
    out_ws->ws_row = rows;
    out_ws->ws_xpixel = 0;
    out_ws->ws_ypixel = 0;

    kernel::tty::TtyService::instance().request_render(kernel::tty::TtyService::RenderReason::Output);

    return 0;
}

extern "C" int tty_set_winsz(tty_handle_t* tty, const yos_winsize_t* ws) {
    kernel::term::Term* term = tty_term_ptr(tty);
    if (!term || !ws) {
        return -1;
    }

    if (term->set_winsz(ws->ws_col, ws->ws_row) != 0) {
        return -1;
    }

    kernel::tty::TtyService::instance().request_render(kernel::tty::TtyService::RenderReason::Resize);

    return 0;
}

extern "C" int tty_scroll(tty_handle_t* tty, int delta) {
    kernel::term::Term* term = tty_term_ptr(tty);
    if (!term) {
        return -1;
    }

    int rc = term->scroll(delta);
    if (rc == 0) {
        kernel::tty::TtyService::instance().request_render(kernel::tty::TtyService::RenderReason::Scroll);
    }

    return rc;
}

extern "C" void tty_render_tick(tty_handle_t* tty) {
    (void)tty;
}

extern "C" void tty_force_redraw_active(void) {
    tty_handle_t* active = kernel::tty::TtyService::instance().get_active_for_render();
    kernel::term::Term* term = tty_term_ptr(active);
    if (term) {
        term->invalidate_view();
    }

    kernel::tty::TtyService::instance().request_render(kernel::tty::TtyService::RenderReason::ActiveChanged);
}

extern "C" tty_handle_t* tty_get_active_for_render(void) {
    return kernel::tty::TtyService::instance().get_active_for_render();
}

extern "C" void* tty_backend_ptr(tty_handle_t* tty) {
    return (void*)tty_term_ptr(tty);
}

extern "C" void tty_render_wakeup(void) {
    kernel::tty::TtyService::instance().request_render(kernel::tty::TtyService::RenderReason::Output);
}

extern "C" void tty_render_wait(void) {
    kernel::tty::TtyService::instance().render_wait();
}

extern "C" int tty_render_try_acquire(void) {
    return kernel::tty::TtyService::instance().render_try_acquire();
}
