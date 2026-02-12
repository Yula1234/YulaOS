// SPDX-License-Identifier: GPL-2.0

#include "netd_hmac_sha256.h"

#include <string.h>

void netd_hmac_sha256_init(netd_hmac_sha256_t* h, const void* key, uint32_t key_len) {
    if (!h) {
        return;
    }

    uint8_t k0[64];
    memset(k0, 0, sizeof(k0));

    if (key_len > 64u) {
        uint8_t tmp[32];
        netd_sha256_hash(key, key_len, tmp);
        memcpy(k0, tmp, (uint32_t)sizeof(tmp));
    } else if (key_len > 0 && key) {
        memcpy(k0, key, key_len);
    }

    uint8_t ipad[64];
    for (uint32_t i = 0; i < 64u; i++) {
        ipad[i] = (uint8_t)(k0[i] ^ 0x36u);
        h->opad[i] = (uint8_t)(k0[i] ^ 0x5Cu);
    }

    netd_sha256_init(&h->inner);
    netd_sha256_update(&h->inner, ipad, (uint32_t)sizeof(ipad));
}

void netd_hmac_sha256_update(netd_hmac_sha256_t* h, const void* data, uint32_t len) {
    if (!h) {
        return;
    }
    netd_sha256_update(&h->inner, data, len);
}

void netd_hmac_sha256_final(netd_hmac_sha256_t* h, uint8_t out[32]) {
    if (!h || !out) {
        return;
    }

    uint8_t inner_hash[32];
    netd_sha256_final(&h->inner, inner_hash);

    netd_sha256_t outer;
    netd_sha256_init(&outer);
    netd_sha256_update(&outer, h->opad, (uint32_t)sizeof(h->opad));
    netd_sha256_update(&outer, inner_hash, (uint32_t)sizeof(inner_hash));
    netd_sha256_final(&outer, out);

    memset(inner_hash, 0, sizeof(inner_hash));
    memset(h->opad, 0, sizeof(h->opad));
    netd_sha256_init(&h->inner);
}

void netd_hmac_sha256(const void* key, uint32_t key_len, const void* data, uint32_t len, uint8_t out[32]) {
    if (!out) {
        return;
    }

    netd_hmac_sha256_t h;
    netd_hmac_sha256_init(&h, key, key_len);
    netd_hmac_sha256_update(&h, data, len);
    netd_hmac_sha256_final(&h, out);
}

