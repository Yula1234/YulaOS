// SPDX-License-Identifier: GPL-2.0

#ifndef YOS_NETWORKD_HMAC_SHA256_H
#define YOS_NETWORKD_HMAC_SHA256_H

#include <stdint.h>

#include "netd_sha256.h"

typedef struct {
    netd_sha256_t inner;
    uint8_t opad[64];
} netd_hmac_sha256_t;

void netd_hmac_sha256_init(netd_hmac_sha256_t* h, const void* key, uint32_t key_len);
void netd_hmac_sha256_update(netd_hmac_sha256_t* h, const void* data, uint32_t len);
void netd_hmac_sha256_final(netd_hmac_sha256_t* h, uint8_t out[32]);

void netd_hmac_sha256(const void* key, uint32_t key_len, const void* data, uint32_t len, uint8_t out[32]);

#endif
