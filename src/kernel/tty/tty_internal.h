#ifndef KERNEL_TTY_INTERNAL_H
#define KERNEL_TTY_INTERNAL_H

#include <kernel/tty/tty_api.h>

#include <kernel/term/term.h>

struct tty_handle {
    kernel::term::Term* term;
};

static inline kernel::term::Term* tty_term_ptr(tty_handle_t* tty) {
    return tty ? tty->term : nullptr;
}

static inline const kernel::term::Term* tty_term_ptr_const(const tty_handle_t* tty) {
    return tty ? tty->term : nullptr;
}

#ifdef __cplusplus
extern "C" {
#endif

tty_handle_t* tty_get_active_for_render(void);
void* tty_backend_ptr(tty_handle_t* tty);

void tty_render_wakeup(void);
void tty_render_wait(void);
int tty_render_try_acquire(void);

#ifdef __cplusplus
}
#endif

#endif
