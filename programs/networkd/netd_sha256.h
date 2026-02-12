// SPDX-License-Identifier: GPL-2.0

#ifndef YOS_NETWORKD_SHA256_H
#define YOS_NETWORKD_SHA256_H

#include <stdint.h>

typedef struct {
    uint32_t h[8];
    uint64_t total_bits;
    uint8_t buf[64];
    uint32_t buf_len;
} netd_sha256_t;

void netd_sha256_init(netd_sha256_t* s);
void netd_sha256_update(netd_sha256_t* s, const void* data, uint32_t len);
void netd_sha256_final(netd_sha256_t* s, uint8_t out[32]);

void netd_sha256_hash(const void* data, uint32_t len, uint8_t out[32]);

#endif
