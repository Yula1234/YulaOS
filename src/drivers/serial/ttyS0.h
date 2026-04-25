#pragma once

#include <kernel/waitq/poll_waitq.h>

#ifdef __cplusplus
extern "C" {
#endif

struct task;

void ttyS0_init(void);

#ifdef __cplusplus
}
#endif
