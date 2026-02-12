// SPDX-License-Identifier: GPL-2.0

#ifndef YOS_NETWORKD_CHACHA20_H
#define YOS_NETWORKD_CHACHA20_H

#include <stdint.h>

typedef struct {
    uint32_t state[16];
} netd_chacha20_t;

void netd_chacha20_init(netd_chacha20_t* c, const uint8_t key[32], const uint8_t nonce[12], uint32_t counter);

void netd_chacha20_xor(netd_chacha20_t* c, const uint8_t* in, uint8_t* out, uint32_t len);

void netd_chacha20_block(const uint32_t state[16], uint8_t out[64]);

#endif
