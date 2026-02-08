#ifndef KERNEL_TTY_H
#define KERNEL_TTY_H

#include <drivers/vga.h>

void tty_set_terminal(term_instance_t* term);
void tty_term_apply_default_size(term_instance_t* term);
void tty_term_print_locked(term_instance_t* term, const char* text);
void tty_task(void* arg);

#endif
