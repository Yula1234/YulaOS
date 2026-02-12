// SPDX-License-Identifier: GPL-2.0

#ifndef YOS_NETWORKD_RAND_H
#define YOS_NETWORKD_RAND_H

#include <stdint.h>

typedef struct {
    uint8_t state[32];
    uint32_t ctr;
    int seeded;
} netd_rand_t;

void netd_rand_init(netd_rand_t* r);

void netd_rand_stir(netd_rand_t* r, const void* data, uint32_t len);

void netd_rand_bytes(netd_rand_t* r, void* out, uint32_t len);

#endif
