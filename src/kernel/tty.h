#ifndef KERNEL_TTY_H
#define KERNEL_TTY_H

#include <drivers/vga.h>

void tty_set_terminal(term_instance_t* term);
void tty_task(void* arg);

#endif
