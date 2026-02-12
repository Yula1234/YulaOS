// SPDX-License-Identifier: GPL-2.0

#include "netd_aead_chacha20poly1305.h"

#include <string.h>

#include "netd_chacha20.h"
#include "netd_poly1305.h"

static void netd_store_le64(uint8_t out[8], uint64_t v) {
    out[0] = (uint8_t)(v);
    out[1] = (uint8_t)(v >> 8);
    out[2] = (uint8_t)(v >> 16);
    out[3] = (uint8_t)(v >> 24);
    out[4] = (uint8_t)(v >> 32);
    out[5] = (uint8_t)(v >> 40);
    out[6] = (uint8_t)(v >> 48);
    out[7] = (uint8_t)(v >> 56);
}

static uint32_t netd_pad16(uint32_t n) {
    uint32_t r = n & 15u;
    if (r == 0) {
        return 0;
    }
    return 16u - r;
}

static int netd_ct_memeq(const void* a, const void* b, uint32_t n) {
    const uint8_t* pa = (const uint8_t*)a;
    const uint8_t* pb = (const uint8_t*)b;
    uint8_t v = 0;
    for (uint32_t i = 0; i < n; i++) {
        v |= (uint8_t)(pa[i] ^ pb[i]);
    }
    return v == 0;
}

static void netd_poly1305_aead_mac(
    const uint8_t poly_key[32],
    const void* aad,
    uint32_t aad_len,
    const void* ciphertext,
    uint32_t ciphertext_len,
    uint8_t out_tag[16]
) {
    netd_poly1305_t p;
    netd_poly1305_init(&p, poly_key);

    if (aad_len > 0) {
        netd_poly1305_update(&p, aad, aad_len);
        uint32_t pad = netd_pad16(aad_len);
        if (pad > 0) {
            uint8_t zeros[16];
            memset(zeros, 0, sizeof(zeros));
            netd_poly1305_update(&p, zeros, pad);
        }
    }

    if (ciphertext_len > 0) {
        netd_poly1305_update(&p, ciphertext, ciphertext_len);
        uint32_t pad = netd_pad16(ciphertext_len);
        if (pad > 0) {
            uint8_t zeros[16];
            memset(zeros, 0, sizeof(zeros));
            netd_poly1305_update(&p, zeros, pad);
        }
    }

    uint8_t lens[16];
    netd_store_le64(lens + 0, (uint64_t)aad_len);
    netd_store_le64(lens + 8, (uint64_t)ciphertext_len);
    netd_poly1305_update(&p, lens, (uint32_t)sizeof(lens));

    netd_poly1305_final(&p, out_tag);
}

int netd_aead_chacha20poly1305_seal(
    const uint8_t key[32],
    const uint8_t nonce[12],
    const void* aad,
    uint32_t aad_len,
    const void* plaintext,
    uint32_t plaintext_len,
    void* out_ciphertext,
    uint8_t out_tag[16]
) {
    if (!key || !nonce || (!plaintext && plaintext_len != 0) || (!out_ciphertext && plaintext_len != 0) || !out_tag) {
        return 0;
    }

    uint8_t block0[64];
    uint32_t st[16];
    netd_chacha20_t c0;
    netd_chacha20_init(&c0, key, nonce, 0);
    memcpy(st, c0.state, sizeof(st));
    netd_chacha20_block(st, block0);
    memset(st, 0, sizeof(st));

    uint8_t poly_key[32];
    memcpy(poly_key, block0, 32u);
    memset(block0, 0, sizeof(block0));

    netd_chacha20_t c;
    netd_chacha20_init(&c, key, nonce, 1);
    netd_chacha20_xor(&c, (const uint8_t*)plaintext, (uint8_t*)out_ciphertext, plaintext_len);
    memset(&c, 0, sizeof(c));

    netd_poly1305_aead_mac(poly_key, aad, aad_len, out_ciphertext, plaintext_len, out_tag);
    memset(poly_key, 0, sizeof(poly_key));
    return 1;
}

int netd_aead_chacha20poly1305_open(
    const uint8_t key[32],
    const uint8_t nonce[12],
    const void* aad,
    uint32_t aad_len,
    const void* ciphertext,
    uint32_t ciphertext_len,
    const uint8_t tag[16],
    void* out_plaintext
) {
    if (!key || !nonce || (!ciphertext && ciphertext_len != 0) || !tag || (!out_plaintext && ciphertext_len != 0)) {
        return 0;
    }

    uint8_t block0[64];
    uint32_t st[16];
    netd_chacha20_t c0;
    netd_chacha20_init(&c0, key, nonce, 0);
    memcpy(st, c0.state, sizeof(st));
    netd_chacha20_block(st, block0);
    memset(st, 0, sizeof(st));

    uint8_t poly_key[32];
    memcpy(poly_key, block0, 32u);
    memset(block0, 0, sizeof(block0));

    uint8_t expected[16];
    netd_poly1305_aead_mac(poly_key, aad, aad_len, ciphertext, ciphertext_len, expected);
    memset(poly_key, 0, sizeof(poly_key));

    if (!netd_ct_memeq(expected, tag, 16u)) {
        memset(expected, 0, sizeof(expected));
        return 0;
    }
    memset(expected, 0, sizeof(expected));

    netd_chacha20_t c;
    netd_chacha20_init(&c, key, nonce, 1);
    netd_chacha20_xor(&c, (const uint8_t*)ciphertext, (uint8_t*)out_plaintext, ciphertext_len);
    memset(&c, 0, sizeof(c));

    return 1;
}

