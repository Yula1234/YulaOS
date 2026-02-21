#ifndef KERNEL_TTY_BRIDGE_H
#define KERNEL_TTY_BRIDGE_H

#include <stdint.h>

#ifdef __cplusplus
#include <lib/cpp/unique_ptr.h>

extern "C" {
#endif

typedef struct tty_handle tty_handle_t;

tty_handle_t* tty_bridge_create_default(void);
void tty_bridge_destroy(tty_handle_t* tty);
void tty_bridge_set_active(tty_handle_t* tty);

void tty_bridge_print(tty_handle_t* tty, const char* s);
void tty_bridge_putc(tty_handle_t* tty, char c);

void tty_bridge_set_colors(tty_handle_t* tty, uint32_t fg, uint32_t bg);

void tty_bridge_force_redraw_active(void);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus

struct tty_handle_deleter {
    void operator()(tty_handle_t* tty) const noexcept {
        tty_bridge_destroy(tty);
    }
};

using tty_handle_ptr = kernel::unique_ptr<tty_handle_t, tty_handle_deleter>;

#endif

#endif
