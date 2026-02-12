// SPDX-License-Identifier: GPL-2.0

#include "netd_aead_aes128gcm.h"

#include <string.h>

static void netd_store_be32(uint8_t out[4], uint32_t v) {
    out[0] = (uint8_t)(v >> 24);
    out[1] = (uint8_t)(v >> 16);
    out[2] = (uint8_t)(v >> 8);
    out[3] = (uint8_t)(v);
}

static void netd_store_be64(uint8_t out[8], uint64_t v) {
    out[0] = (uint8_t)(v >> 56);
    out[1] = (uint8_t)(v >> 48);
    out[2] = (uint8_t)(v >> 40);
    out[3] = (uint8_t)(v >> 32);
    out[4] = (uint8_t)(v >> 24);
    out[5] = (uint8_t)(v >> 16);
    out[6] = (uint8_t)(v >> 8);
    out[7] = (uint8_t)(v);
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

static uint8_t netd_gf_mul(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    uint8_t x = a;
    uint8_t y = b;
    for (uint32_t i = 0; i < 8u; i++) {
        if ((y & 1u) != 0u) {
            r ^= x;
        }
        uint8_t hi = (uint8_t)(x & 0x80u);
        x = (uint8_t)(x << 1);
        if (hi != 0u) {
            x ^= 0x1Bu;
        }
        y = (uint8_t)(y >> 1);
    }
    return r;
}

static uint8_t netd_aes_sbox(uint8_t x) {
    uint8_t x2 = netd_gf_mul(x, x);
    uint8_t x4 = netd_gf_mul(x2, x2);
    uint8_t x8 = netd_gf_mul(x4, x4);
    uint8_t x16 = netd_gf_mul(x8, x8);
    uint8_t x32 = netd_gf_mul(x16, x16);
    uint8_t x64 = netd_gf_mul(x32, x32);
    uint8_t x128 = netd_gf_mul(x64, x64);

    uint8_t x254 = netd_gf_mul(x128, x64);
    x254 = netd_gf_mul(x254, x32);
    x254 = netd_gf_mul(x254, x16);
    x254 = netd_gf_mul(x254, x8);
    x254 = netd_gf_mul(x254, x4);
    x254 = netd_gf_mul(x254, x2);

    uint8_t inv = x254;

    uint8_t y = inv;
    uint8_t r = (uint8_t)(y ^ (uint8_t)((y << 1) | (y >> 7)));
    r ^= (uint8_t)((y << 2) | (y >> 6));
    r ^= (uint8_t)((y << 3) | (y >> 5));
    r ^= (uint8_t)((y << 4) | (y >> 4));
    r ^= 0x63u;
    return r;
}

static void netd_aes_sub_bytes(uint8_t st[16]) {
    for (uint32_t i = 0; i < 16u; i++) {
        st[i] = netd_aes_sbox(st[i]);
    }
}

static void netd_aes_shift_rows(uint8_t st[16]) {
    uint8_t t[16];

    t[0] = st[0];
    t[1] = st[5];
    t[2] = st[10];
    t[3] = st[15];

    t[4] = st[4];
    t[5] = st[9];
    t[6] = st[14];
    t[7] = st[3];

    t[8] = st[8];
    t[9] = st[13];
    t[10] = st[2];
    t[11] = st[7];

    t[12] = st[12];
    t[13] = st[1];
    t[14] = st[6];
    t[15] = st[11];

    memcpy(st, t, 16u);
    memset(t, 0, sizeof(t));
}

static void netd_aes_mix_columns(uint8_t st[16]) {
    for (uint32_t col = 0; col < 4u; col++) {
        uint32_t i = col * 4u;
        uint8_t a0 = st[i + 0u];
        uint8_t a1 = st[i + 1u];
        uint8_t a2 = st[i + 2u];
        uint8_t a3 = st[i + 3u];

        uint8_t r0 = (uint8_t)(netd_gf_mul(a0, 2u) ^ netd_gf_mul(a1, 3u) ^ a2 ^ a3);
        uint8_t r1 = (uint8_t)(a0 ^ netd_gf_mul(a1, 2u) ^ netd_gf_mul(a2, 3u) ^ a3);
        uint8_t r2 = (uint8_t)(a0 ^ a1 ^ netd_gf_mul(a2, 2u) ^ netd_gf_mul(a3, 3u));
        uint8_t r3 = (uint8_t)(netd_gf_mul(a0, 3u) ^ a1 ^ a2 ^ netd_gf_mul(a3, 2u));

        st[i + 0u] = r0;
        st[i + 1u] = r1;
        st[i + 2u] = r2;
        st[i + 3u] = r3;
    }
}

static void netd_aes_add_round_key(uint8_t st[16], const uint8_t* rk) {
    for (uint32_t i = 0; i < 16u; i++) {
        st[i] ^= rk[i];
    }
}

static void netd_aes128_key_expand(const uint8_t key[16], uint8_t out_rk[176]) {
    static const uint8_t rcon[10] = {
        0x01u, 0x02u, 0x04u, 0x08u, 0x10u,
        0x20u, 0x40u, 0x80u, 0x1Bu, 0x36u
    };

    memcpy(out_rk, key, 16u);

    uint32_t bytes = 16u;
    uint32_t rcon_i = 0;

    uint8_t tmp[4];

    while (bytes < 176u) {
        tmp[0] = out_rk[bytes - 4u];
        tmp[1] = out_rk[bytes - 3u];
        tmp[2] = out_rk[bytes - 2u];
        tmp[3] = out_rk[bytes - 1u];

        if ((bytes & 15u) == 0u) {
            uint8_t t0 = tmp[0];
            tmp[0] = tmp[1];
            tmp[1] = tmp[2];
            tmp[2] = tmp[3];
            tmp[3] = t0;

            tmp[0] = netd_aes_sbox(tmp[0]);
            tmp[1] = netd_aes_sbox(tmp[1]);
            tmp[2] = netd_aes_sbox(tmp[2]);
            tmp[3] = netd_aes_sbox(tmp[3]);

            tmp[0] ^= rcon[rcon_i];
            rcon_i++;
        }

        out_rk[bytes + 0u] = (uint8_t)(out_rk[bytes - 16u + 0u] ^ tmp[0]);
        out_rk[bytes + 1u] = (uint8_t)(out_rk[bytes - 16u + 1u] ^ tmp[1]);
        out_rk[bytes + 2u] = (uint8_t)(out_rk[bytes - 16u + 2u] ^ tmp[2]);
        out_rk[bytes + 3u] = (uint8_t)(out_rk[bytes - 16u + 3u] ^ tmp[3]);
        bytes += 4u;
    }

    memset(tmp, 0, sizeof(tmp));
}

static void netd_aes128_encrypt_block(const uint8_t rk[176], const uint8_t in[16], uint8_t out[16]) {
    uint8_t st[16];
    memcpy(st, in, 16u);

    netd_aes_add_round_key(st, rk);

    for (uint32_t round = 1; round < 10u; round++) {
        netd_aes_sub_bytes(st);
        netd_aes_shift_rows(st);
        netd_aes_mix_columns(st);
        netd_aes_add_round_key(st, rk + round * 16u);
    }

    netd_aes_sub_bytes(st);
    netd_aes_shift_rows(st);
    netd_aes_add_round_key(st, rk + 160u);

    memcpy(out, st, 16u);
    memset(st, 0, sizeof(st));
}

static void netd_gcm_inc32(uint8_t counter[16]) {
    uint32_t v = ((uint32_t)counter[12] << 24) | ((uint32_t)counter[13] << 16) | ((uint32_t)counter[14] << 8) | (uint32_t)counter[15];
    v += 1u;
    counter[12] = (uint8_t)(v >> 24);
    counter[13] = (uint8_t)(v >> 16);
    counter[14] = (uint8_t)(v >> 8);
    counter[15] = (uint8_t)(v);
}

static void netd_gf128_shift_right(uint8_t v[16]) {
    uint8_t carry = 0;
    for (uint32_t i = 0; i < 16u; i++) {
        uint8_t b = v[i];
        uint8_t new_carry = (uint8_t)(b & 1u);
        v[i] = (uint8_t)((b >> 1) | (carry << 7));
        carry = new_carry;
    }
}

static void netd_gf128_mul(const uint8_t x[16], const uint8_t y[16], uint8_t out[16]) {
    uint8_t z[16];
    memset(z, 0, sizeof(z));

    uint8_t v[16];
    memcpy(v, y, 16u);

    for (uint32_t i = 0; i < 128u; i++) {
        uint8_t xi = (uint8_t)((x[i / 8u] >> (7u - (i & 7u))) & 1u);
        if (xi != 0u) {
            for (uint32_t b = 0; b < 16u; b++) {
                z[b] ^= v[b];
            }
        }

        uint8_t lsb = (uint8_t)(v[15] & 1u);
        netd_gf128_shift_right(v);
        if (lsb != 0u) {
            v[0] ^= 0xE1u;
        }
    }

    memcpy(out, z, 16u);
    memset(z, 0, sizeof(z));
    memset(v, 0, sizeof(v));
}

static void netd_ghash_update(uint8_t y[16], const uint8_t h[16], const uint8_t block[16]) {
    uint8_t x[16];
    for (uint32_t i = 0; i < 16u; i++) {
        x[i] = (uint8_t)(y[i] ^ block[i]);
    }

    netd_gf128_mul(x, h, y);
    memset(x, 0, sizeof(x));
}

static void netd_gcm_ghash(
    const uint8_t h[16],
    const void* aad,
    uint32_t aad_len,
    const void* c,
    uint32_t c_len,
    uint8_t out_y[16]
) {
    uint8_t y[16];
    memset(y, 0, sizeof(y));

    const uint8_t* pa = (const uint8_t*)aad;
    uint32_t aoff = 0;
    while (aoff < aad_len) {
        uint8_t block[16];
        uint32_t take = aad_len - aoff;
        if (take > 16u) {
            take = 16u;
        }
        memset(block, 0, sizeof(block));
        memcpy(block, pa + aoff, take);
        netd_ghash_update(y, h, block);
        memset(block, 0, sizeof(block));
        aoff += take;
    }

    const uint8_t* pc = (const uint8_t*)c;
    uint32_t coff = 0;
    while (coff < c_len) {
        uint8_t block[16];
        uint32_t take = c_len - coff;
        if (take > 16u) {
            take = 16u;
        }
        memset(block, 0, sizeof(block));
        memcpy(block, pc + coff, take);
        netd_ghash_update(y, h, block);
        memset(block, 0, sizeof(block));
        coff += take;
    }

    uint8_t len_block[16];
    uint64_t a_bits = (uint64_t)aad_len * 8u;
    uint64_t c_bits = (uint64_t)c_len * 8u;
    netd_store_be64(len_block + 0, a_bits);
    netd_store_be64(len_block + 8, c_bits);
    netd_ghash_update(y, h, len_block);
    memset(len_block, 0, sizeof(len_block));

    memcpy(out_y, y, 16u);
    memset(y, 0, sizeof(y));
}

static void netd_aes128_ctr_xor(
    const uint8_t rk[176],
    const uint8_t nonce[12],
    uint32_t counter,
    const uint8_t* in,
    uint8_t* out,
    uint32_t len
) {
    uint8_t ctr[16];
    memcpy(ctr + 0, nonce, 12u);
    netd_store_be32(ctr + 12, counter);

    uint32_t off = 0;
    while (off < len) {
        uint8_t ks[16];
        netd_aes128_encrypt_block(rk, ctr, ks);

        uint32_t take = len - off;
        if (take > 16u) {
            take = 16u;
        }
        for (uint32_t i = 0; i < take; i++) {
            out[off + i] = (uint8_t)(in[off + i] ^ ks[i]);
        }

        memset(ks, 0, sizeof(ks));

        counter += 1u;
        netd_store_be32(ctr + 12, counter);
        off += take;
    }

    memset(ctr, 0, sizeof(ctr));
}

int netd_aead_aes128gcm_seal(
    const uint8_t key[16],
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

    uint8_t rk[176];
    netd_aes128_key_expand(key, rk);

    uint8_t zero[16];
    memset(zero, 0, sizeof(zero));

    uint8_t h[16];
    netd_aes128_encrypt_block(rk, zero, h);

    uint8_t j0[16];
    memcpy(j0 + 0, nonce, 12u);
    j0[12] = 0;
    j0[13] = 0;
    j0[14] = 0;
    j0[15] = 1;

    uint32_t counter = 2u;
    netd_aes128_ctr_xor(
        rk,
        nonce,
        counter,
        (const uint8_t*)plaintext,
        (uint8_t*)out_ciphertext,
        plaintext_len
    );

    uint8_t s[16];
    netd_gcm_ghash(h, aad, aad_len, out_ciphertext, plaintext_len, s);

    uint8_t e0[16];
    netd_aes128_encrypt_block(rk, j0, e0);

    for (uint32_t i = 0; i < 16u; i++) {
        out_tag[i] = (uint8_t)(e0[i] ^ s[i]);
    }

    memset(rk, 0, sizeof(rk));
    memset(zero, 0, sizeof(zero));
    memset(h, 0, sizeof(h));
    memset(j0, 0, sizeof(j0));
    memset(s, 0, sizeof(s));
    memset(e0, 0, sizeof(e0));
    return 1;
}

int netd_aead_aes128gcm_open(
    const uint8_t key[16],
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

    uint8_t rk[176];
    netd_aes128_key_expand(key, rk);

    uint8_t zero[16];
    memset(zero, 0, sizeof(zero));

    uint8_t h[16];
    netd_aes128_encrypt_block(rk, zero, h);

    uint8_t j0[16];
    memcpy(j0 + 0, nonce, 12u);
    j0[12] = 0;
    j0[13] = 0;
    j0[14] = 0;
    j0[15] = 1;

    uint8_t s[16];
    netd_gcm_ghash(h, aad, aad_len, ciphertext, ciphertext_len, s);

    uint8_t e0[16];
    netd_aes128_encrypt_block(rk, j0, e0);

    uint8_t expected[16];
    for (uint32_t i = 0; i < 16u; i++) {
        expected[i] = (uint8_t)(e0[i] ^ s[i]);
    }

    if (!netd_ct_memeq(expected, tag, 16u)) {
        memset(rk, 0, sizeof(rk));
        memset(zero, 0, sizeof(zero));
        memset(h, 0, sizeof(h));
        memset(j0, 0, sizeof(j0));
        memset(s, 0, sizeof(s));
        memset(e0, 0, sizeof(e0));
        memset(expected, 0, sizeof(expected));
        return 0;
    }

    uint32_t counter = 2u;
    netd_aes128_ctr_xor(
        rk,
        nonce,
        counter,
        (const uint8_t*)ciphertext,
        (uint8_t*)out_plaintext,
        ciphertext_len
    );

    memset(rk, 0, sizeof(rk));
    memset(zero, 0, sizeof(zero));
    memset(h, 0, sizeof(h));
    memset(j0, 0, sizeof(j0));
    memset(s, 0, sizeof(s));
    memset(e0, 0, sizeof(e0));
    memset(expected, 0, sizeof(expected));
    return 1;
}
