#ifndef KERNEL_TTY_API_H
#define KERNEL_TTY_API_H

#include <stdint.h>

#include <yos/ioctl.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tty_handle tty_handle_t;

tty_handle_t* tty_create_default(void);
void tty_set_active(tty_handle_t* tty);

void tty_print(tty_handle_t* tty, const char* s);
void tty_putc(tty_handle_t* tty, char c);

void tty_set_colors(tty_handle_t* tty, uint32_t fg, uint32_t bg);

void tty_force_redraw_active(void);

#ifdef __cplusplus
}
#endif

#endif
