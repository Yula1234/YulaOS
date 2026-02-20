#ifndef KERNEL_TTY_INTERNAL_H
#define KERNEL_TTY_INTERNAL_H

#include <kernel/tty/tty_session.h>

struct tty_handle;
using tty_handle_t = tty_handle;

struct tty_handle {
    kernel::tty::TtySession* session;
};

static inline kernel::tty::TtySession* tty_session_ptr(tty_handle_t* tty) {
    return tty ? tty->session : nullptr;
}

static inline const kernel::tty::TtySession* tty_session_ptr_const(
    const tty_handle_t* tty
) {
    return tty ? tty->session : nullptr;
}

static inline kernel::term::Term* tty_term_ptr(tty_handle_t* tty) {
    kernel::tty::TtySession* session = tty_session_ptr(tty);
    return session ? session->term() : nullptr;
}

static inline const kernel::term::Term* tty_term_ptr_const(
    const tty_handle_t* tty
) {
    const kernel::tty::TtySession* session = tty_session_ptr_const(tty);
    return session ? session->term() : nullptr;
}

#endif
