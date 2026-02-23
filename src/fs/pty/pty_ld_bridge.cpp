#include "pty_ld_bridge.h"

#include <kernel/tty/line_discipline.h>

#include <mm/heap.h>

#include <lib/cpp/new.h>

namespace {

static kernel::tty::LineDisciplineConfig cfg_from_termios(const yos_termios_t& t) {
    kernel::tty::LineDisciplineConfig cfg;

    cfg.canonical = (t.c_lflag & YOS_LFLAG_ICANON) != 0;
    cfg.echo = (t.c_lflag & YOS_LFLAG_ECHO) != 0;
    cfg.isig = (t.c_lflag & YOS_LFLAG_ISIG) != 0;

    cfg.igncr = (t.c_iflag & YOS_IFLAG_IGNCR) != 0;
    cfg.icrnl = (t.c_iflag & YOS_IFLAG_ICRNL) != 0;
    cfg.inlcr = (t.c_iflag & YOS_IFLAG_INLCR) != 0;

    cfg.opost = (t.c_oflag & YOS_OFLAG_OPOST) != 0;
    cfg.onlcr = (t.c_oflag & YOS_OFLAG_ONLCR) != 0;

    cfg.vmin = t.c_cc[YOS_VMIN];
    cfg.vtime = t.c_cc[YOS_VTIME];

    cfg.vintr = t.c_cc[YOS_VINTR];
    cfg.vquit = t.c_cc[YOS_VQUIT];
    cfg.vsusp = t.c_cc[YOS_VSUSP];

    return cfg;
}

}

struct pty_ld_handle {
    kernel::tty::LineDiscipline ld;

    pty_ld_emit_fn echo_emit = nullptr;
    void* echo_ctx = nullptr;

    pty_ld_signal_fn sig_emit = nullptr;
    void* sig_ctx = nullptr;
};

static size_t echo_emit_wrapper(const uint8_t* data, size_t size, void* ctx) {
    auto* self = static_cast<pty_ld_handle_t*>(ctx);
    if (!self || !self->echo_emit) {
        return 0;
    }

    return self->echo_emit(data, size, self->echo_ctx);
}

static void signal_emit_wrapper(int sig, void* ctx) {
    auto* self = static_cast<pty_ld_handle_t*>(ctx);
    if (!self || !self->sig_emit) {
        return;
    }

    self->sig_emit(sig, self->sig_ctx);
}

extern "C" pty_ld_handle_t* pty_ld_create(
    const yos_termios_t* termios,
    pty_ld_emit_fn echo_emit,
    void* echo_ctx,
    pty_ld_signal_fn sig_emit,
    void* sig_ctx
) {
    if (!termios) {
        return nullptr;
    }

    void* mem = kmalloc(sizeof(pty_ld_handle_t));
    if (!mem) {
        return nullptr;
    }

    auto* h = new (mem) pty_ld_handle_t;

    h->echo_emit = echo_emit;
    h->echo_ctx = echo_ctx;

    h->sig_emit = sig_emit;
    h->sig_ctx = sig_ctx;

    h->ld.set_echo_emitter(echo_emit_wrapper, h);
    h->ld.set_signal_emitter(signal_emit_wrapper, h);

    (void)pty_ld_set_termios(h, termios);

    return h;
}

extern "C" void pty_ld_destroy(pty_ld_handle_t* h) {
    if (!h) {
        return;
    }

    h->~pty_ld_handle_t();
    kfree(h);
}

extern "C" int pty_ld_set_termios(pty_ld_handle_t* h, const yos_termios_t* termios) {
    if (!h || !termios) {
        return -1;
    }

    h->ld.set_config(cfg_from_termios(*termios));
    return 0;
}

extern "C" void pty_ld_receive(pty_ld_handle_t* h, const uint8_t* data, size_t size) {
    if (!h) {
        return;
    }

    h->ld.receive_bytes(data, size);
}

extern "C" size_t pty_ld_read(pty_ld_handle_t* h, void* out, size_t size) {
    if (!h) {
        return 0;
    }

    return h->ld.read(out, size);
}

extern "C" size_t pty_ld_write(pty_ld_handle_t* h, const void* in, size_t size) {
    if (!h || !h->echo_emit) {
        return 0;
    }

    return h->ld.write_transform(in, size, echo_emit_wrapper, h);
}

extern "C" int pty_ld_has_readable(pty_ld_handle_t* h) {
    if (!h) {
        return 0;
    }

    return h->ld.has_readable() ? 1 : 0;
}
