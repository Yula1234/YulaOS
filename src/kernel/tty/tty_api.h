#ifndef KERNEL_TTY_API_H
#define KERNEL_TTY_API_H

#include <stdint.h>

#include <yos/ioctl.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tty_handle tty_handle_t;

tty_handle_t* tty_create_default(void);
void tty_destroy(tty_handle_t* tty);

void tty_set_active(tty_handle_t* tty);

void tty_write(tty_handle_t* tty, const char* buf, uint32_t len);
void tty_print(tty_handle_t* tty, const char* s);

void tty_putc(tty_handle_t* tty, char c);

void tty_set_colors(tty_handle_t* tty, uint32_t fg, uint32_t bg);

int tty_get_winsz(tty_handle_t* tty, yos_winsize_t* out_ws);
int tty_set_winsz(tty_handle_t* tty, const yos_winsize_t* ws);

int tty_scroll(tty_handle_t* tty, int delta);

void tty_render_tick(tty_handle_t* tty);

void tty_force_redraw_active(void);

#ifdef __cplusplus
}
#endif

#endif
