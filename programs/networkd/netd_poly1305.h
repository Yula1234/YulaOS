// SPDX-License-Identifier: GPL-2.0

#ifndef YOS_NETWORKD_POLY1305_H
#define YOS_NETWORKD_POLY1305_H

#include <stdint.h>

typedef struct {
    uint32_t r[5];
    uint32_t h[5];
    uint32_t pad[4];
    uint8_t buf[16];
    uint32_t buf_len;
} netd_poly1305_t;

void netd_poly1305_init(netd_poly1305_t* p, const uint8_t key[32]);
void netd_poly1305_update(netd_poly1305_t* p, const void* data, uint32_t len);
void netd_poly1305_final(netd_poly1305_t* p, uint8_t out[16]);

#endif
