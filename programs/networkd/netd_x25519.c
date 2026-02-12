// SPDX-License-Identifier: GPL-2.0

#include "netd_x25519.h"

#include <string.h>

typedef struct {
    int32_t v[10];
} netd_fe25519_t;

static uint64_t netd_load64_24_le(const uint8_t* in) {
    return ((uint64_t)in[0])
        | ((uint64_t)in[1] << 8)
        | ((uint64_t)in[2] << 16);
}

static uint64_t netd_load64_32_le(const uint8_t* in) {
    return ((uint64_t)in[0])
        | ((uint64_t)in[1] << 8)
        | ((uint64_t)in[2] << 16)
        | ((uint64_t)in[3] << 24);
}

static void netd_fe_0(netd_fe25519_t* f) {
    memset(f, 0, sizeof(*f));
}

static void netd_fe_1(netd_fe25519_t* f) {
    netd_fe_0(f);
    f->v[0] = 1;
}

static void netd_fe_copy(netd_fe25519_t* dst, const netd_fe25519_t* src) {
    memcpy(dst, src, sizeof(*dst));
}

static void netd_fe_add(netd_fe25519_t* out, const netd_fe25519_t* a, const netd_fe25519_t* b) {
    for (int i = 0; i < 10; i++) {
        out->v[i] = a->v[i] + b->v[i];
    }
}

static void netd_fe_sub(netd_fe25519_t* out, const netd_fe25519_t* a, const netd_fe25519_t* b) {
    for (int i = 0; i < 10; i++) {
        out->v[i] = a->v[i] - b->v[i];
    }
}

static void netd_fe_cswap(netd_fe25519_t* a, netd_fe25519_t* b, uint32_t swap) {
    int32_t mask = -(int32_t)swap;
    for (int i = 0; i < 10; i++) {
        int32_t x = a->v[i] ^ b->v[i];
        x &= mask;
        a->v[i] ^= x;
        b->v[i] ^= x;
    }
}

static void netd_fe_mul(netd_fe25519_t* out, const netd_fe25519_t* a, const netd_fe25519_t* b) {
    int64_t f0 = (int64_t)a->v[0];
    int64_t f1 = (int64_t)a->v[1];
    int64_t f2 = (int64_t)a->v[2];
    int64_t f3 = (int64_t)a->v[3];
    int64_t f4 = (int64_t)a->v[4];
    int64_t f5 = (int64_t)a->v[5];
    int64_t f6 = (int64_t)a->v[6];
    int64_t f7 = (int64_t)a->v[7];
    int64_t f8 = (int64_t)a->v[8];
    int64_t f9 = (int64_t)a->v[9];

    int64_t g0 = (int64_t)b->v[0];
    int64_t g1 = (int64_t)b->v[1];
    int64_t g2 = (int64_t)b->v[2];
    int64_t g3 = (int64_t)b->v[3];
    int64_t g4 = (int64_t)b->v[4];
    int64_t g5 = (int64_t)b->v[5];
    int64_t g6 = (int64_t)b->v[6];
    int64_t g7 = (int64_t)b->v[7];
    int64_t g8 = (int64_t)b->v[8];
    int64_t g9 = (int64_t)b->v[9];

    int64_t f1_2 = f1 * 2;
    int64_t f3_2 = f3 * 2;
    int64_t f5_2 = f5 * 2;
    int64_t f7_2 = f7 * 2;
    int64_t f9_2 = f9 * 2;

    int64_t g1_19 = g1 * 19;
    int64_t g2_19 = g2 * 19;
    int64_t g3_19 = g3 * 19;
    int64_t g4_19 = g4 * 19;
    int64_t g5_19 = g5 * 19;
    int64_t g6_19 = g6 * 19;
    int64_t g7_19 = g7 * 19;
    int64_t g8_19 = g8 * 19;
    int64_t g9_19 = g9 * 19;

    int64_t h0 = f0 * g0
        + f1_2 * g9_19
        + f2 * g8_19
        + f3_2 * g7_19
        + f4 * g6_19
        + f5_2 * g5_19
        + f6 * g4_19
        + f7_2 * g3_19
        + f8 * g2_19
        + f9_2 * g1_19;

    int64_t h1 = f0 * g1
        + f1 * g0
        + f2 * g9_19
        + f3 * g8_19
        + f4 * g7_19
        + f5 * g6_19
        + f6 * g5_19
        + f7 * g4_19
        + f8 * g3_19
        + f9 * g2_19;

    int64_t h2 = f0 * g2
        + f1_2 * g1
        + f2 * g0
        + f3_2 * g9_19
        + f4 * g8_19
        + f5_2 * g7_19
        + f6 * g6_19
        + f7_2 * g5_19
        + f8 * g4_19
        + f9_2 * g3_19;

    int64_t h3 = f0 * g3
        + f1 * g2
        + f2 * g1
        + f3 * g0
        + f4 * g9_19
        + f5 * g8_19
        + f6 * g7_19
        + f7 * g6_19
        + f8 * g5_19
        + f9 * g4_19;

    int64_t h4 = f0 * g4
        + f1_2 * g3
        + f2 * g2
        + f3_2 * g1
        + f4 * g0
        + f5_2 * g9_19
        + f6 * g8_19
        + f7_2 * g7_19
        + f8 * g6_19
        + f9_2 * g5_19;

    int64_t h5 = f0 * g5
        + f1 * g4
        + f2 * g3
        + f3 * g2
        + f4 * g1
        + f5 * g0
        + f6 * g9_19
        + f7 * g8_19
        + f8 * g7_19
        + f9 * g6_19;

    int64_t h6 = f0 * g6
        + f1_2 * g5
        + f2 * g4
        + f3_2 * g3
        + f4 * g2
        + f5_2 * g1
        + f6 * g0
        + f7_2 * g9_19
        + f8 * g8_19
        + f9_2 * g7_19;

    int64_t h7 = f0 * g7
        + f1 * g6
        + f2 * g5
        + f3 * g4
        + f4 * g3
        + f5 * g2
        + f6 * g1
        + f7 * g0
        + f8 * g9_19
        + f9 * g8_19;

    int64_t h8 = f0 * g8
        + f1_2 * g7
        + f2 * g6
        + f3_2 * g5
        + f4 * g4
        + f5_2 * g3
        + f6 * g2
        + f7_2 * g1
        + f8 * g0
        + f9_2 * g9_19;

    int64_t h9 = f0 * g9
        + f1 * g8
        + f2 * g7
        + f3 * g6
        + f4 * g5
        + f5 * g4
        + f6 * g3
        + f7 * g2
        + f8 * g1
        + f9 * g0;

    int64_t carry0 = (h0 + (1LL << 25)) >> 26;
    h1 += carry0;
    h0 -= carry0 << 26;

    int64_t carry4 = (h4 + (1LL << 25)) >> 26;
    h5 += carry4;
    h4 -= carry4 << 26;

    int64_t carry1 = (h1 + (1LL << 24)) >> 25;
    h2 += carry1;
    h1 -= carry1 << 25;

    int64_t carry5 = (h5 + (1LL << 24)) >> 25;
    h6 += carry5;
    h5 -= carry5 << 25;

    int64_t carry2 = (h2 + (1LL << 25)) >> 26;
    h3 += carry2;
    h2 -= carry2 << 26;

    int64_t carry6 = (h6 + (1LL << 25)) >> 26;
    h7 += carry6;
    h6 -= carry6 << 26;

    int64_t carry3 = (h3 + (1LL << 24)) >> 25;
    h4 += carry3;
    h3 -= carry3 << 25;

    int64_t carry7 = (h7 + (1LL << 24)) >> 25;
    h8 += carry7;
    h7 -= carry7 << 25;

    carry4 = (h4 + (1LL << 25)) >> 26;
    h5 += carry4;
    h4 -= carry4 << 26;

    int64_t carry8 = (h8 + (1LL << 25)) >> 26;
    h9 += carry8;
    h8 -= carry8 << 26;

    int64_t carry9 = (h9 + (1LL << 24)) >> 25;
    h0 += carry9 * 19;
    h9 -= carry9 << 25;

    carry0 = (h0 + (1LL << 25)) >> 26;
    h1 += carry0;
    h0 -= carry0 << 26;

    carry1 = (h1 + (1LL << 24)) >> 25;
    h2 += carry1;
    h1 -= carry1 << 25;

    out->v[0] = (int32_t)h0;
    out->v[1] = (int32_t)h1;
    out->v[2] = (int32_t)h2;
    out->v[3] = (int32_t)h3;
    out->v[4] = (int32_t)h4;
    out->v[5] = (int32_t)h5;
    out->v[6] = (int32_t)h6;
    out->v[7] = (int32_t)h7;
    out->v[8] = (int32_t)h8;
    out->v[9] = (int32_t)h9;
}

static void netd_fe_sqr(netd_fe25519_t* out, const netd_fe25519_t* a) {
    netd_fe_mul(out, a, a);
}

static void netd_fe_mul_small(netd_fe25519_t* out, const netd_fe25519_t* a, int32_t k) {
    netd_fe25519_t b;
    netd_fe_0(&b);
    b.v[0] = k;
    netd_fe_mul(out, a, &b);
}

static void netd_fe_inv(netd_fe25519_t* out, const netd_fe25519_t* z) {
    if (!out || !z) {
        return;
    }

    netd_fe25519_t c;
    netd_fe_1(&c);

    for (int a = 254; a >= 0; a--) {
        netd_fe_sqr(&c, &c);

        if (a != 2 && a != 4) {
            netd_fe_mul(&c, &c, z);
        }
    }

    netd_fe_copy(out, &c);
    memset(&c, 0, sizeof(c));
}

static void netd_fe_from_u25519(netd_fe25519_t* out, const uint8_t in[32]) {
    uint8_t s[32];
    memcpy(s, in, 32u);
    s[31] &= 127u;

    int64_t h0 = (int64_t)netd_load64_32_le(&s[0]);
    int64_t h1 = (int64_t)netd_load64_24_le(&s[4]) << 6;
    int64_t h2 = (int64_t)netd_load64_24_le(&s[7]) << 5;
    int64_t h3 = (int64_t)netd_load64_24_le(&s[10]) << 3;
    int64_t h4 = (int64_t)netd_load64_24_le(&s[13]) << 2;
    int64_t h5 = (int64_t)netd_load64_32_le(&s[16]);
    int64_t h6 = (int64_t)netd_load64_24_le(&s[20]) << 7;
    int64_t h7 = (int64_t)netd_load64_24_le(&s[23]) << 5;
    int64_t h8 = (int64_t)netd_load64_24_le(&s[26]) << 4;
    int64_t h9 = ((int64_t)netd_load64_24_le(&s[29]) & 0x7FFFFFu) << 2;

    int64_t carry9 = (h9 + (1LL << 24)) >> 25;
    h0 += carry9 * 19;
    h9 -= carry9 << 25;

    int64_t carry1 = (h1 + (1LL << 24)) >> 25;
    h2 += carry1;
    h1 -= carry1 << 25;

    int64_t carry3 = (h3 + (1LL << 24)) >> 25;
    h4 += carry3;
    h3 -= carry3 << 25;

    int64_t carry5 = (h5 + (1LL << 24)) >> 25;
    h6 += carry5;
    h5 -= carry5 << 25;

    int64_t carry7 = (h7 + (1LL << 24)) >> 25;
    h8 += carry7;
    h7 -= carry7 << 25;

    int64_t carry0 = (h0 + (1LL << 25)) >> 26;
    h1 += carry0;
    h0 -= carry0 << 26;

    int64_t carry2 = (h2 + (1LL << 25)) >> 26;
    h3 += carry2;
    h2 -= carry2 << 26;

    int64_t carry4 = (h4 + (1LL << 25)) >> 26;
    h5 += carry4;
    h4 -= carry4 << 26;

    int64_t carry6 = (h6 + (1LL << 25)) >> 26;
    h7 += carry6;
    h6 -= carry6 << 26;

    int64_t carry8 = (h8 + (1LL << 25)) >> 26;
    h9 += carry8;
    h8 -= carry8 << 26;

    out->v[0] = (int32_t)h0;
    out->v[1] = (int32_t)h1;
    out->v[2] = (int32_t)h2;
    out->v[3] = (int32_t)h3;
    out->v[4] = (int32_t)h4;
    out->v[5] = (int32_t)h5;
    out->v[6] = (int32_t)h6;
    out->v[7] = (int32_t)h7;
    out->v[8] = (int32_t)h8;
    out->v[9] = (int32_t)h9;

    memset(s, 0, sizeof(s));
}

static int64_t netd_fe_div_floor(int64_t v, int64_t base) {
    if (v >= 0) {
        return v / base;
    }
    return -(((-v) + base - 1) / base);
}

static void netd_fe_carry_pass_for_bytes(int64_t h[10]) {
    const int64_t base26 = 1LL << 26;
    const int64_t base25 = 1LL << 25;

    int64_t carry0 = netd_fe_div_floor(h[0], base26);
    h[1] += carry0;
    h[0] -= carry0 * base26;

    int64_t carry1 = netd_fe_div_floor(h[1], base25);
    h[2] += carry1;
    h[1] -= carry1 * base25;

    int64_t carry2 = netd_fe_div_floor(h[2], base26);
    h[3] += carry2;
    h[2] -= carry2 * base26;

    int64_t carry3 = netd_fe_div_floor(h[3], base25);
    h[4] += carry3;
    h[3] -= carry3 * base25;

    int64_t carry4 = netd_fe_div_floor(h[4], base26);
    h[5] += carry4;
    h[4] -= carry4 * base26;

    int64_t carry5 = netd_fe_div_floor(h[5], base25);
    h[6] += carry5;
    h[5] -= carry5 * base25;

    int64_t carry6 = netd_fe_div_floor(h[6], base26);
    h[7] += carry6;
    h[6] -= carry6 * base26;

    int64_t carry7 = netd_fe_div_floor(h[7], base25);
    h[8] += carry7;
    h[7] -= carry7 * base25;

    int64_t carry8 = netd_fe_div_floor(h[8], base26);
    h[9] += carry8;
    h[8] -= carry8 * base26;

    int64_t carry9 = netd_fe_div_floor(h[9], base25);
    h[0] += carry9 * 19;
    h[9] -= carry9 * base25;

    carry0 = netd_fe_div_floor(h[0], base26);
    h[1] += carry0;
    h[0] -= carry0 * base26;

    carry1 = netd_fe_div_floor(h[1], base25);
    h[2] += carry1;
    h[1] -= carry1 * base25;
}

static int netd_u25519_is_ge_p(const uint8_t a[32]) {
    static const uint8_t p[32] = {
        0xEDu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu,
        0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu,
        0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu,
        0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0x7Fu
    };

    for (int i = 31; i >= 0; i--) {
        if (a[i] > p[i]) {
            return 1;
        }
        if (a[i] < p[i]) {
            return 0;
        }
    }
    return 1;
}

static void netd_u25519_sub_p(uint8_t a[32]) {
    static const uint8_t p[32] = {
        0xEDu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu,
        0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu,
        0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu,
        0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0x7Fu
    };

    uint32_t borrow = 0;
    for (int i = 0; i < 32; i++) {
        uint32_t ai = (uint32_t)a[i];
        uint32_t sub = (uint32_t)p[i] + borrow;

        if (ai < sub) {
            a[i] = (uint8_t)(ai + 256u - sub);
            borrow = 1u;
        } else {
            a[i] = (uint8_t)(ai - sub);
            borrow = 0u;
        }
    }
}

static void netd_fe_to_u25519(uint8_t out[32], netd_fe25519_t* f) {
    int64_t h[10];
    h[0] = (int64_t)f->v[0];
    h[1] = (int64_t)f->v[1];
    h[2] = (int64_t)f->v[2];
    h[3] = (int64_t)f->v[3];
    h[4] = (int64_t)f->v[4];
    h[5] = (int64_t)f->v[5];
    h[6] = (int64_t)f->v[6];
    h[7] = (int64_t)f->v[7];
    h[8] = (int64_t)f->v[8];
    h[9] = (int64_t)f->v[9];

    netd_fe_carry_pass_for_bytes(h);

    int64_t q = (19 * h[9] + (1LL << 24)) >> 25;
    q = (h[0] + q) >> 26;
    q = (h[1] + q) >> 25;
    q = (h[2] + q) >> 26;
    q = (h[3] + q) >> 25;
    q = (h[4] + q) >> 26;
    q = (h[5] + q) >> 25;
    q = (h[6] + q) >> 26;
    q = (h[7] + q) >> 25;
    q = (h[8] + q) >> 26;
    q = (h[9] + q) >> 25;

    h[0] += 19 * q;

    netd_fe_carry_pass_for_bytes(h);

    uint64_t t0 = (uint64_t)h[0] & 0x3FFFFFFu;
    uint64_t t1 = (uint64_t)h[1] & 0x1FFFFFFu;
    uint64_t t2 = (uint64_t)h[2] & 0x3FFFFFFu;
    uint64_t t3 = (uint64_t)h[3] & 0x1FFFFFFu;
    uint64_t t4 = (uint64_t)h[4] & 0x3FFFFFFu;
    uint64_t t5 = (uint64_t)h[5] & 0x1FFFFFFu;
    uint64_t t6 = (uint64_t)h[6] & 0x3FFFFFFu;
    uint64_t t7 = (uint64_t)h[7] & 0x1FFFFFFu;
    uint64_t t8 = (uint64_t)h[8] & 0x3FFFFFFu;
    uint64_t t9 = (uint64_t)h[9] & 0x1FFFFFFu;

    out[0] = (uint8_t)(t0 >> 0);
    out[1] = (uint8_t)(t0 >> 8);
    out[2] = (uint8_t)(t0 >> 16);
    out[3] = (uint8_t)((t0 >> 24) | (t1 << 2));
    out[4] = (uint8_t)(t1 >> 6);
    out[5] = (uint8_t)(t1 >> 14);
    out[6] = (uint8_t)((t1 >> 22) | (t2 << 3));
    out[7] = (uint8_t)(t2 >> 5);
    out[8] = (uint8_t)(t2 >> 13);
    out[9] = (uint8_t)((t2 >> 21) | (t3 << 5));
    out[10] = (uint8_t)(t3 >> 3);
    out[11] = (uint8_t)(t3 >> 11);
    out[12] = (uint8_t)((t3 >> 19) | (t4 << 6));
    out[13] = (uint8_t)(t4 >> 2);
    out[14] = (uint8_t)(t4 >> 10);
    out[15] = (uint8_t)(t4 >> 18);
    out[16] = (uint8_t)(t5 >> 0);
    out[17] = (uint8_t)(t5 >> 8);
    out[18] = (uint8_t)(t5 >> 16);
    out[19] = (uint8_t)((t5 >> 24) | (t6 << 1));
    out[20] = (uint8_t)(t6 >> 7);
    out[21] = (uint8_t)(t6 >> 15);
    out[22] = (uint8_t)((t6 >> 23) | (t7 << 3));
    out[23] = (uint8_t)(t7 >> 5);
    out[24] = (uint8_t)(t7 >> 13);
    out[25] = (uint8_t)((t7 >> 21) | (t8 << 4));
    out[26] = (uint8_t)(t8 >> 4);
    out[27] = (uint8_t)(t8 >> 12);
    out[28] = (uint8_t)((t8 >> 20) | (t9 << 6));
    out[29] = (uint8_t)(t9 >> 2);
    out[30] = (uint8_t)(t9 >> 10);
    out[31] = (uint8_t)(t9 >> 18);
    out[31] &= 127u;

    if (netd_u25519_is_ge_p(out)) {
        netd_u25519_sub_p(out);
    }
}

static void netd_x25519_scalar_clamp(uint8_t k[32]) {
    k[0] &= 248u;
    k[31] &= 127u;
    k[31] |= 64u;
}

void netd_x25519(uint8_t out_shared[32], const uint8_t priv[32], const uint8_t peer_pub[32]) {
    if (!out_shared || !priv || !peer_pub) {
        return;
    }

    uint8_t k[32];
    memcpy(k, priv, sizeof(k));
    netd_x25519_scalar_clamp(k);

    netd_fe25519_t x1;
    netd_fe_from_u25519(&x1, peer_pub);

    netd_fe25519_t x2;
    netd_fe25519_t z2;
    netd_fe25519_t x3;
    netd_fe25519_t z3;

    netd_fe_1(&x2);
    netd_fe_0(&z2);
    netd_fe_copy(&x3, &x1);
    netd_fe_1(&z3);

    uint32_t swap = 0;

    netd_fe25519_t a;
    netd_fe25519_t aa;
    netd_fe25519_t b;
    netd_fe25519_t bb;
    netd_fe25519_t e;
    netd_fe25519_t c;
    netd_fe25519_t d;
    netd_fe25519_t da;
    netd_fe25519_t cb;
    netd_fe25519_t t0;
    netd_fe25519_t t1;

    for (int t = 254; t >= 0; t--) {
        uint32_t bit = (uint32_t)((k[t / 8] >> (t & 7)) & 1u);
        swap ^= bit;
        netd_fe_cswap(&x2, &x3, swap);
        netd_fe_cswap(&z2, &z3, swap);
        swap = bit;

        netd_fe_add(&a, &x2, &z2);
        netd_fe_sqr(&aa, &a);
        netd_fe_sub(&b, &x2, &z2);
        netd_fe_sqr(&bb, &b);
        netd_fe_sub(&e, &aa, &bb);

        netd_fe_add(&c, &x3, &z3);
        netd_fe_sub(&d, &x3, &z3);

        netd_fe_mul(&da, &d, &a);
        netd_fe_mul(&cb, &c, &b);

        netd_fe_add(&t0, &da, &cb);
        netd_fe_sub(&t1, &da, &cb);

        netd_fe_sqr(&x3, &t0);
        netd_fe_sqr(&t0, &t1);
        netd_fe_mul(&z3, &t0, &x1);

        netd_fe_mul(&x2, &aa, &bb);

        netd_fe_mul_small(&t0, &e, 121665);
        netd_fe_add(&t0, &t0, &aa);
        netd_fe_mul(&z2, &e, &t0);
    }

    netd_fe_cswap(&x2, &x3, swap);
    netd_fe_cswap(&z2, &z3, swap);

    netd_fe25519_t z2_inv;
    netd_fe_inv(&z2_inv, &z2);

    netd_fe25519_t x;
    netd_fe_mul(&x, &x2, &z2_inv);
    netd_fe_to_u25519(out_shared, &x);

    memset(&x, 0, sizeof(x));
    memset(&z2_inv, 0, sizeof(z2_inv));
    memset(&x1, 0, sizeof(x1));
    memset(k, 0, sizeof(k));
}

void netd_x25519_public_key(uint8_t out_pub[32], const uint8_t priv[32]) {
    if (!out_pub || !priv) {
        return;
    }

    uint8_t base[32];
    memset(base, 0, sizeof(base));
    base[0] = 9u;
    netd_x25519(out_pub, priv, base);
    memset(base, 0, sizeof(base));
}

