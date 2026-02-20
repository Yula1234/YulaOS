#ifndef KERNEL_TTY_H
#define KERNEL_TTY_H

#include <kernel/tty/tty_api.h>

#ifdef __cplusplus
extern "C" {
#endif

void tty_task(void* arg);

#ifdef __cplusplus
}
#endif

#endif
