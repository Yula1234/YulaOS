// SPDX-License-Identifier: GPL-2.0

#include "netd_poly1305.h"

#include <string.h>

static uint32_t netd_poly1305_load_le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void netd_poly1305_store_le32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void netd_poly1305_block(netd_poly1305_t* st, const uint8_t m[16], uint32_t hibit) {
    uint64_t t0 = (uint64_t)netd_poly1305_load_le32(m + 0);
    uint64_t t1 = (uint64_t)netd_poly1305_load_le32(m + 4);
    uint64_t t2 = (uint64_t)netd_poly1305_load_le32(m + 8);
    uint64_t t3 = (uint64_t)netd_poly1305_load_le32(m + 12);

    uint32_t m0 = (uint32_t)(t0 & 0x3FFFFFFu);
    uint32_t m1 = (uint32_t)(((t0 >> 26) | (t1 << 6)) & 0x3FFFFFFu);
    uint32_t m2 = (uint32_t)(((t1 >> 20) | (t2 << 12)) & 0x3FFFFFFu);
    uint32_t m3 = (uint32_t)(((t2 >> 14) | (t3 << 18)) & 0x3FFFFFFu);
    uint32_t m4 = (uint32_t)((t3 >> 8) & 0x00FFFFFFu);
    m4 |= hibit;

    st->h[0] += m0;
    st->h[1] += m1;
    st->h[2] += m2;
    st->h[3] += m3;
    st->h[4] += m4;

    uint64_t r0 = st->r[0];
    uint64_t r1 = st->r[1];
    uint64_t r2 = st->r[2];
    uint64_t r3 = st->r[3];
    uint64_t r4 = st->r[4];

    uint64_t s1 = r1 * 5u;
    uint64_t s2 = r2 * 5u;
    uint64_t s3 = r3 * 5u;
    uint64_t s4 = r4 * 5u;

    uint64_t h0 = st->h[0];
    uint64_t h1 = st->h[1];
    uint64_t h2 = st->h[2];
    uint64_t h3 = st->h[3];
    uint64_t h4 = st->h[4];

    uint64_t d0 = (h0 * r0) + (h1 * s4) + (h2 * s3) + (h3 * s2) + (h4 * s1);
    uint64_t d1 = (h0 * r1) + (h1 * r0) + (h2 * s4) + (h3 * s3) + (h4 * s2);
    uint64_t d2 = (h0 * r2) + (h1 * r1) + (h2 * r0) + (h3 * s4) + (h4 * s3);
    uint64_t d3 = (h0 * r3) + (h1 * r2) + (h2 * r1) + (h3 * r0) + (h4 * s4);
    uint64_t d4 = (h0 * r4) + (h1 * r3) + (h2 * r2) + (h3 * r1) + (h4 * r0);

    uint64_t c = d0 >> 26;
    st->h[0] = (uint32_t)(d0 & 0x3FFFFFFu);
    d1 += c;
    c = d1 >> 26;
    st->h[1] = (uint32_t)(d1 & 0x3FFFFFFu);
    d2 += c;
    c = d2 >> 26;
    st->h[2] = (uint32_t)(d2 & 0x3FFFFFFu);
    d3 += c;
    c = d3 >> 26;
    st->h[3] = (uint32_t)(d3 & 0x3FFFFFFu);
    d4 += c;
    c = d4 >> 26;
    st->h[4] = (uint32_t)(d4 & 0x3FFFFFFu);

    st->h[0] += (uint32_t)(c * 5u);
    c = (uint64_t)st->h[0] >> 26;
    st->h[0] &= 0x3FFFFFFu;
    st->h[1] += (uint32_t)c;
}

void netd_poly1305_init(netd_poly1305_t* st, const uint8_t key[32]) {
    if (!st || !key) {
        return;
    }

    memset(st, 0, sizeof(*st));

    uint64_t t0 = (uint64_t)netd_poly1305_load_le32(key + 0);
    uint64_t t1 = (uint64_t)netd_poly1305_load_le32(key + 4);
    uint64_t t2 = (uint64_t)netd_poly1305_load_le32(key + 8);
    uint64_t t3 = (uint64_t)netd_poly1305_load_le32(key + 12);

    uint32_t r0 = (uint32_t)(t0 & 0x3FFFFFFu);
    uint32_t r1 = (uint32_t)(((t0 >> 26) | (t1 << 6)) & 0x3FFFF03u);
    uint32_t r2 = (uint32_t)(((t1 >> 20) | (t2 << 12)) & 0x3FFC0FFu);
    uint32_t r3 = (uint32_t)(((t2 >> 14) | (t3 << 18)) & 0x3F03FFFu);
    uint32_t r4 = (uint32_t)((t3 >> 8) & 0x00FFFFFu);

    st->r[0] = r0;
    st->r[1] = r1;
    st->r[2] = r2;
    st->r[3] = r3;
    st->r[4] = r4;

    st->pad[0] = netd_poly1305_load_le32(key + 16);
    st->pad[1] = netd_poly1305_load_le32(key + 20);
    st->pad[2] = netd_poly1305_load_le32(key + 24);
    st->pad[3] = netd_poly1305_load_le32(key + 28);
}

void netd_poly1305_update(netd_poly1305_t* st, const void* data, uint32_t len) {
    if (!st || (!data && len != 0)) {
        return;
    }

    const uint8_t* p = (const uint8_t*)data;
    while (len > 0) {
        if (st->buf_len == 0 && len >= 16u) {
            netd_poly1305_block(st, p, 1u << 24);
            p += 16u;
            len -= 16u;
            continue;
        }

        uint32_t take = 16u - st->buf_len;
        if (take > len) {
            take = len;
        }

        memcpy(st->buf + st->buf_len, p, take);
        st->buf_len += take;
        p += take;
        len -= take;

        if (st->buf_len == 16u) {
            netd_poly1305_block(st, st->buf, 1u << 24);
            st->buf_len = 0;
            memset(st->buf, 0, sizeof(st->buf));
        }
    }
}

void netd_poly1305_final(netd_poly1305_t* st, uint8_t out[16]) {
    if (!st || !out) {
        return;
    }

    if (st->buf_len > 0) {
        uint8_t last[16];
        memset(last, 0, sizeof(last));
        memcpy(last, st->buf, st->buf_len);
        last[st->buf_len] = 0x01u;
        netd_poly1305_block(st, last, 0);
        memset(last, 0, sizeof(last));
    }

    uint32_t h0 = st->h[0];
    uint32_t h1 = st->h[1];
    uint32_t h2 = st->h[2];
    uint32_t h3 = st->h[3];
    uint32_t h4 = st->h[4];

    uint32_t c = h1 >> 26;
    h1 &= 0x3FFFFFFu;
    h2 += c;
    c = h2 >> 26;
    h2 &= 0x3FFFFFFu;
    h3 += c;
    c = h3 >> 26;
    h3 &= 0x3FFFFFFu;
    h4 += c;
    c = h4 >> 26;
    h4 &= 0x3FFFFFFu;
    h0 += c * 5u;
    c = h0 >> 26;
    h0 &= 0x3FFFFFFu;
    h1 += c;

    uint32_t g0 = h0 + 5u;
    c = g0 >> 26;
    g0 &= 0x3FFFFFFu;

    uint32_t g1 = h1 + c;
    c = g1 >> 26;
    g1 &= 0x3FFFFFFu;

    uint32_t g2 = h2 + c;
    c = g2 >> 26;
    g2 &= 0x3FFFFFFu;

    uint32_t g3 = h3 + c;
    c = g3 >> 26;
    g3 &= 0x3FFFFFFu;

    uint32_t g4 = h4 + c;
    g4 -= (1u << 26);

    uint32_t mask = (g4 >> 31) - 1u;
    uint32_t inv_mask = ~mask;

    h0 = (h0 & inv_mask) | (g0 & mask);
    h1 = (h1 & inv_mask) | (g1 & mask);
    h2 = (h2 & inv_mask) | (g2 & mask);
    h3 = (h3 & inv_mask) | (g3 & mask);
    h4 = (h4 & inv_mask) | (g4 & mask);

    uint64_t f0 = ((uint64_t)h0) | ((uint64_t)h1 << 26);
    uint64_t f1 = ((uint64_t)h1 >> 6) | ((uint64_t)h2 << 20);
    uint64_t f2 = ((uint64_t)h2 >> 12) | ((uint64_t)h3 << 14);
    uint64_t f3 = ((uint64_t)h3 >> 18) | ((uint64_t)h4 << 8);

    uint64_t t = f0 + st->pad[0];
    uint32_t o0 = (uint32_t)t;
    t = f1 + st->pad[1] + (t >> 32);
    uint32_t o1 = (uint32_t)t;
    t = f2 + st->pad[2] + (t >> 32);
    uint32_t o2 = (uint32_t)t;
    t = f3 + st->pad[3] + (t >> 32);
    uint32_t o3 = (uint32_t)t;

    netd_poly1305_store_le32(out + 0, o0);
    netd_poly1305_store_le32(out + 4, o1);
    netd_poly1305_store_le32(out + 8, o2);
    netd_poly1305_store_le32(out + 12, o3);

    memset(st, 0, sizeof(*st));
}

