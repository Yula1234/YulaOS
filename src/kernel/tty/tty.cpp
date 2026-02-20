#include <kernel/tty/tty_internal.h>

#include <drivers/fbdev.h>

#include <hal/lock.h>

#include <lib/cpp/semaphore.h>

#include <lib/cpp/new.h>

#include <lib/string.h>
#include <mm/heap.h>

namespace {

class TtyService {
public:
    TtyService()
        : m_active_lock()
        , m_active(0)
        , m_render_sem(0)
        , m_init_state(0) {
    }

    void ensure_init() {
        int state = __atomic_load_n(&m_init_state, __ATOMIC_ACQUIRE);
        if (state == 2) {
            return;
        }

        if (state == 0) {
            int expected = 0;
            if (__atomic_compare_exchange_n(&m_init_state, &expected, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                spinlock_init(&m_active_lock);
                __atomic_store_n(&m_init_state, 2, __ATOMIC_RELEASE);
                return;
            }
        }

        while (__atomic_load_n(&m_init_state, __ATOMIC_ACQUIRE) != 2) {
        }
    }

    tty_handle_t* get_active_for_render() {
        ensure_init();

        uint32_t flags = spinlock_acquire_safe(&m_active_lock);
        tty_handle_t* cur = m_active;
        spinlock_release_safe(&m_active_lock, flags);

        return cur;
    }

    void set_active(tty_handle_t* tty) {
        ensure_init();

        uint32_t flags = spinlock_acquire_safe(&m_active_lock);
        m_active = tty;
        spinlock_release_safe(&m_active_lock, flags);
    }

    void clear_active_if_matches(tty_handle_t* tty) {
        ensure_init();

        uint32_t flags = spinlock_acquire_safe(&m_active_lock);
        if (m_active == tty) {
            m_active = 0;
        }
        spinlock_release_safe(&m_active_lock, flags);
    }

    void render_wakeup() {
        ensure_init();
        m_render_sem.signal();
    }

    void render_wait() {
        ensure_init();
        m_render_sem.wait();
    }

    int render_try_acquire() {
        ensure_init();
        return m_render_sem.try_acquire() ? 1 : 0;
    }

private:
    spinlock_t m_active_lock;
    tty_handle_t* m_active;
    kernel::Semaphore m_render_sem;
    int m_init_state;
};

static TtyService g_tty_service;

}

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

    kernel::term::Term* term = new (kernel::nothrow) kernel::term::Term(cols, view_rows);
    if (!term) {
        kfree(tty);
        return 0;
    }

    tty->term = term;

    return tty;
}

extern "C" void tty_destroy(tty_handle_t* tty) {
    if (!tty) {
        return;
    }

    g_tty_service.clear_active_if_matches(tty);

    if (tty->term) {
        delete tty->term;
        tty->term = nullptr;
    }

    kfree(tty);
}

extern "C" void tty_set_active(tty_handle_t* tty) {
    g_tty_service.set_active(tty);
    g_tty_service.render_wakeup();
}

extern "C" void tty_write(tty_handle_t* tty, const char* buf, uint32_t len) {
    kernel::term::Term* term = tty_term_ptr(tty);
    if (!term || !buf || len == 0) {
        return;
    }

    term->write(buf, len);

    tty_render_wakeup();
}

extern "C" void tty_print(tty_handle_t* tty, const char* s) {
    kernel::term::Term* term = tty_term_ptr(tty);
    if (!term || !s) {
        return;
    }

    term->print(s);

    tty_render_wakeup();
}

extern "C" void tty_putc(tty_handle_t* tty, char c) {
    kernel::term::Term* term = tty_term_ptr(tty);
    if (!term) {
        return;
    }

    term->putc(c);

    tty_render_wakeup();
}

extern "C" void tty_set_colors(tty_handle_t* tty, uint32_t fg, uint32_t bg) {
    kernel::term::Term* term = tty_term_ptr(tty);
    if (!term) {
        return;
    }

    term->set_colors(fg, bg);

    tty_render_wakeup();
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

    tty_render_wakeup();

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

    tty_render_wakeup();

    return 0;
}

extern "C" int tty_scroll(tty_handle_t* tty, int delta) {
    kernel::term::Term* term = tty_term_ptr(tty);
    if (!term) {
        return -1;
    }

    int rc = term->scroll(delta);
    if (rc == 0) {
        tty_render_wakeup();
    }

    return rc;
}

extern "C" void tty_render_tick(tty_handle_t* tty) {
    (void)tty;
}

extern "C" tty_handle_t* tty_get_active_for_render(void) {
    return g_tty_service.get_active_for_render();
}

extern "C" void* tty_backend_ptr(tty_handle_t* tty) {
    return (void*)tty_term_ptr(tty);
}

extern "C" void tty_render_wakeup(void) {
    g_tty_service.render_wakeup();
}

extern "C" void tty_render_wait(void) {
    g_tty_service.render_wait();
}

extern "C" int tty_render_try_acquire(void) {
    return g_tty_service.render_try_acquire();
}
