// SPDX-License-Identifier: GPL-2.0

#include "netd_sha256.h"

#include <string.h>

static uint32_t netd_sha256_rotr(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32u - n));
}

static uint32_t netd_sha256_ch(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ (~x & z);
}

static uint32_t netd_sha256_maj(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ (x & z) ^ (y & z);
}

static uint32_t netd_sha256_bsig0(uint32_t x) {
    return netd_sha256_rotr(x, 2) ^ netd_sha256_rotr(x, 13) ^ netd_sha256_rotr(x, 22);
}

static uint32_t netd_sha256_bsig1(uint32_t x) {
    return netd_sha256_rotr(x, 6) ^ netd_sha256_rotr(x, 11) ^ netd_sha256_rotr(x, 25);
}

static uint32_t netd_sha256_ssig0(uint32_t x) {
    return netd_sha256_rotr(x, 7) ^ netd_sha256_rotr(x, 18) ^ (x >> 3);
}

static uint32_t netd_sha256_ssig1(uint32_t x) {
    return netd_sha256_rotr(x, 17) ^ netd_sha256_rotr(x, 19) ^ (x >> 10);
}

static uint32_t netd_sha256_load_be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void netd_sha256_store_be32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v);
}

static void netd_sha256_compress(netd_sha256_t* s, const uint8_t block[64]) {
    static const uint32_t k[64] = {
        0x428A2F98u, 0x71374491u, 0xB5C0FBCFu, 0xE9B5DBA5u, 0x3956C25Bu, 0x59F111F1u, 0x923F82A4u, 0xAB1C5ED5u,
        0xD807AA98u, 0x12835B01u, 0x243185BEu, 0x550C7DC3u, 0x72BE5D74u, 0x80DEB1FEu, 0x9BDC06A7u, 0xC19BF174u,
        0xE49B69C1u, 0xEFBE4786u, 0x0FC19DC6u, 0x240CA1CCu, 0x2DE92C6Fu, 0x4A7484AAu, 0x5CB0A9DCu, 0x76F988DAu,
        0x983E5152u, 0xA831C66Du, 0xB00327C8u, 0xBF597FC7u, 0xC6E00BF3u, 0xD5A79147u, 0x06CA6351u, 0x14292967u,
        0x27B70A85u, 0x2E1B2138u, 0x4D2C6DFCu, 0x53380D13u, 0x650A7354u, 0x766A0ABBu, 0x81C2C92Eu, 0x92722C85u,
        0xA2BFE8A1u, 0xA81A664Bu, 0xC24B8B70u, 0xC76C51A3u, 0xD192E819u, 0xD6990624u, 0xF40E3585u, 0x106AA070u,
        0x19A4C116u, 0x1E376C08u, 0x2748774Cu, 0x34B0BCB5u, 0x391C0CB3u, 0x4ED8AA4Au, 0x5B9CCA4Fu, 0x682E6FF3u,
        0x748F82EEu, 0x78A5636Fu, 0x84C87814u, 0x8CC70208u, 0x90BEFFFAu, 0xA4506CEBu, 0xBEF9A3F7u, 0xC67178F2u
    };

    uint32_t w[64];
    for (uint32_t i = 0; i < 16u; i++) {
        w[i] = netd_sha256_load_be32(block + i * 4u);
    }
    for (uint32_t i = 16u; i < 64u; i++) {
        w[i] = netd_sha256_ssig1(w[i - 2u]) + w[i - 7u] + netd_sha256_ssig0(w[i - 15u]) + w[i - 16u];
    }

    uint32_t a = s->h[0];
    uint32_t b = s->h[1];
    uint32_t c = s->h[2];
    uint32_t d = s->h[3];
    uint32_t e = s->h[4];
    uint32_t f = s->h[5];
    uint32_t g = s->h[6];
    uint32_t h = s->h[7];

    for (uint32_t i = 0; i < 64u; i++) {
        uint32_t t1 = h + netd_sha256_bsig1(e) + netd_sha256_ch(e, f, g) + k[i] + w[i];
        uint32_t t2 = netd_sha256_bsig0(a) + netd_sha256_maj(a, b, c);

        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    s->h[0] += a;
    s->h[1] += b;
    s->h[2] += c;
    s->h[3] += d;
    s->h[4] += e;
    s->h[5] += f;
    s->h[6] += g;
    s->h[7] += h;
}

void netd_sha256_init(netd_sha256_t* s) {
    if (!s) {
        return;
    }

    s->h[0] = 0x6A09E667u;
    s->h[1] = 0xBB67AE85u;
    s->h[2] = 0x3C6EF372u;
    s->h[3] = 0xA54FF53Au;
    s->h[4] = 0x510E527Fu;
    s->h[5] = 0x9B05688Cu;
    s->h[6] = 0x1F83D9ABu;
    s->h[7] = 0x5BE0CD19u;

    s->total_bits = 0;
    s->buf_len = 0;
    memset(s->buf, 0, sizeof(s->buf));
}

void netd_sha256_update(netd_sha256_t* s, const void* data, uint32_t len) {
    if (!s || (!data && len != 0)) {
        return;
    }

    const uint8_t* p = (const uint8_t*)data;
    s->total_bits += (uint64_t)len * 8u;

    while (len > 0) {
        uint32_t space = 64u - s->buf_len;
        uint32_t take = len;
        if (take > space) {
            take = space;
        }

        memcpy(s->buf + s->buf_len, p, take);
        s->buf_len += take;
        p += take;
        len -= take;

        if (s->buf_len == 64u) {
            netd_sha256_compress(s, s->buf);
            s->buf_len = 0;
            memset(s->buf, 0, sizeof(s->buf));
        }
    }
}

void netd_sha256_final(netd_sha256_t* s, uint8_t out[32]) {
    if (!s || !out) {
        return;
    }

    uint64_t bits = s->total_bits;

    uint8_t pad[64];
    memset(pad, 0, sizeof(pad));
    pad[0] = 0x80u;

    uint32_t pad_len = 0;
    if (s->buf_len < 56u) {
        pad_len = 56u - s->buf_len;
    } else {
        pad_len = 64u + 56u - s->buf_len;
    }

    netd_sha256_update(s, pad, pad_len);

    uint8_t len_be[8];
    len_be[0] = (uint8_t)(bits >> 56);
    len_be[1] = (uint8_t)(bits >> 48);
    len_be[2] = (uint8_t)(bits >> 40);
    len_be[3] = (uint8_t)(bits >> 32);
    len_be[4] = (uint8_t)(bits >> 24);
    len_be[5] = (uint8_t)(bits >> 16);
    len_be[6] = (uint8_t)(bits >> 8);
    len_be[7] = (uint8_t)(bits);

    netd_sha256_update(s, len_be, (uint32_t)sizeof(len_be));

    for (uint32_t i = 0; i < 8u; i++) {
        netd_sha256_store_be32(out + i * 4u, s->h[i]);
    }

    netd_sha256_init(s);
}

void netd_sha256_hash(const void* data, uint32_t len, uint8_t out[32]) {
    if (!out) {
        return;
    }

    netd_sha256_t s;
    netd_sha256_init(&s);
    netd_sha256_update(&s, data, len);
    netd_sha256_final(&s, out);
}
