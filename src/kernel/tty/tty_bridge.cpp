#include <kernel/tty/tty_bridge.h>

#include <kernel/tty/tty_internal.h>
#include <kernel/tty/tty_service.h>

#include <drivers/fbdev.h>

#include <lib/cpp/new.h>

#include <mm/heap.h>

namespace {

void tty_default_size(int& out_cols, int& out_view_rows) {
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

}

extern "C" tty_handle_t* tty_bridge_create_default(void) {
    int cols = 0;
    int view_rows = 0;
    tty_default_size(cols, view_rows);

    kernel::tty::TtySession* session = kernel::tty::TtySession::create(cols, view_rows);
    if (!session) {
        return nullptr;
    }

    tty_handle_t* tty = new (kernel::nothrow) tty_handle_t(session);
    if (!tty) {
        delete session;
        return nullptr;
    }

    return tty;
}

extern "C" void tty_bridge_destroy(tty_handle_t* tty) {
    if (!tty) {
        return;
    }

    kernel::tty::TtyService::instance().clear_active_if_matches(tty);
    delete tty;
}

extern "C" void tty_bridge_set_active(tty_handle_t* tty) {
    kernel::tty::TtyService::instance().set_active(tty);

    kernel::tty::TtyService::instance().request_render(
        kernel::tty::TtyService::RenderReason::ActiveChanged
    );
}

extern "C" void tty_bridge_print(tty_handle_t* tty, const char* s) {
    kernel::term::Term* term = tty_term_ptr(tty);
    if (!term || !s) {
        return;
    }

    term->print(s);

    kernel::tty::TtyService::instance().request_render(
        kernel::tty::TtyService::RenderReason::Output
    );
}

extern "C" void tty_bridge_putc(tty_handle_t* tty, char c) {
    kernel::term::Term* term = tty_term_ptr(tty);
    if (!term) {
        return;
    }

    term->putc(c);

    kernel::tty::TtyService::instance().request_render(
        kernel::tty::TtyService::RenderReason::Output
    );
}

extern "C" void tty_bridge_set_colors(tty_handle_t* tty, uint32_t fg, uint32_t bg) {
    kernel::term::Term* term = tty_term_ptr(tty);
    if (!term) {
        return;
    }

    term->set_colors(fg, bg);

    kernel::tty::TtyService::instance().request_render(
        kernel::tty::TtyService::RenderReason::Output
    );
}

extern "C" void tty_bridge_force_redraw_active(void) {
    tty_handle_t* active = kernel::tty::TtyService::instance().get_active_for_render();
    kernel::term::Term* term = tty_term_ptr(active);
    if (term) {
        term->invalidate_view();
    }

    kernel::tty::TtyService::instance().request_render(
        kernel::tty::TtyService::RenderReason::ActiveChanged
    );
}
