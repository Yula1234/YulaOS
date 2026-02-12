// SPDX-License-Identifier: GPL-2.0

#include "netd_chacha20.h"

#include <string.h>

static uint32_t netd_chacha20_rotl(uint32_t x, uint32_t n) {
    return (x << n) | (x >> (32u - n));
}

static uint32_t netd_chacha20_load_le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void netd_chacha20_store_le32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void netd_chacha20_qr(uint32_t* a, uint32_t* b, uint32_t* c, uint32_t* d) {
    *a += *b;
    *d ^= *a;
    *d = netd_chacha20_rotl(*d, 16);

    *c += *d;
    *b ^= *c;
    *b = netd_chacha20_rotl(*b, 12);

    *a += *b;
    *d ^= *a;
    *d = netd_chacha20_rotl(*d, 8);

    *c += *d;
    *b ^= *c;
    *b = netd_chacha20_rotl(*b, 7);
}

void netd_chacha20_block(const uint32_t state[16], uint8_t out[64]) {
    uint32_t x[16];
    memcpy(x, state, sizeof(x));

    for (uint32_t i = 0; i < 10u; i++) {
        netd_chacha20_qr(&x[0], &x[4], &x[8], &x[12]);
        netd_chacha20_qr(&x[1], &x[5], &x[9], &x[13]);
        netd_chacha20_qr(&x[2], &x[6], &x[10], &x[14]);
        netd_chacha20_qr(&x[3], &x[7], &x[11], &x[15]);

        netd_chacha20_qr(&x[0], &x[5], &x[10], &x[15]);
        netd_chacha20_qr(&x[1], &x[6], &x[11], &x[12]);
        netd_chacha20_qr(&x[2], &x[7], &x[8], &x[13]);
        netd_chacha20_qr(&x[3], &x[4], &x[9], &x[14]);
    }

    for (uint32_t i = 0; i < 16u; i++) {
        x[i] += state[i];
        netd_chacha20_store_le32(out + i * 4u, x[i]);
    }

    memset(x, 0, sizeof(x));
}

void netd_chacha20_init(netd_chacha20_t* c, const uint8_t key[32], const uint8_t nonce[12], uint32_t counter) {
    if (!c || !key || !nonce) {
        return;
    }

    c->state[0] = 0x61707865u;
    c->state[1] = 0x3320646Eu;
    c->state[2] = 0x79622D32u;
    c->state[3] = 0x6B206574u;

    for (uint32_t i = 0; i < 8u; i++) {
        c->state[4u + i] = netd_chacha20_load_le32(key + i * 4u);
    }

    c->state[12] = counter;
    c->state[13] = netd_chacha20_load_le32(nonce + 0u);
    c->state[14] = netd_chacha20_load_le32(nonce + 4u);
    c->state[15] = netd_chacha20_load_le32(nonce + 8u);
}

void netd_chacha20_xor(netd_chacha20_t* c, const uint8_t* in, uint8_t* out, uint32_t len) {
    if (!c || (!in && len != 0) || (!out && len != 0)) {
        return;
    }

    uint32_t off = 0;
    while (off < len) {
        uint8_t block[64];
        netd_chacha20_block(c->state, block);

        uint32_t take = len - off;
        if (take > 64u) {
            take = 64u;
        }

        for (uint32_t i = 0; i < take; i++) {
            out[off + i] = (uint8_t)(in[off + i] ^ block[i]);
        }

        off += take;
        c->state[12] += 1u;
    }
}

