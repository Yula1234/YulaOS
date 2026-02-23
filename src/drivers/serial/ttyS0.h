#pragma once

#include <kernel/poll_waitq.h>

#ifdef __cplusplus
extern "C" {
#endif

struct task;

void ttyS0_init(void);

int ttyS0_poll_ready(void);

int ttyS0_poll_waitq_register(poll_waiter_t* w, struct task* task);

#ifdef __cplusplus
}
#endif
