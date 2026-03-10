#ifndef KERNEL_FUTEX_FUTEX_H
#define KERNEL_FUTEX_FUTEX_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int futex_wait(uint32_t key, volatile const uint32_t* uaddr, uint32_t expected);

int futex_wake(uint32_t key, uint32_t max_wake);

#ifdef __cplusplus
}
#endif

#endif
