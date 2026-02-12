// SPDX-License-Identifier: GPL-2.0

#include "netd_hkdf_sha256.h"

#include <string.h>

#include "netd_hmac_sha256.h"

void netd_hkdf_sha256_extract(const void* salt, uint32_t salt_len, const void* ikm, uint32_t ikm_len, uint8_t out_prk[32]) {
    if (!out_prk) {
        return;
    }

    uint8_t zero_salt[32];
    if (!salt) {
        memset(zero_salt, 0, sizeof(zero_salt));
        salt = zero_salt;
        salt_len = (uint32_t)sizeof(zero_salt);
    }

    netd_hmac_sha256(salt, salt_len, ikm, ikm_len, out_prk);
}

int netd_hkdf_sha256_expand(const uint8_t prk[32], const void* info, uint32_t info_len, void* out, uint32_t out_len) {
    if (!prk || (!out && out_len != 0)) {
        return 0;
    }

    if (out_len == 0) {
        return 1;
    }

    uint8_t t[32];
    uint8_t ctr = 1;

    uint8_t* dst = (uint8_t*)out;
    uint32_t produced = 0;
    uint32_t t_len = 0;

    while (produced < out_len) {
        netd_hmac_sha256_t h;
        netd_hmac_sha256_init(&h, prk, 32u);
        if (t_len > 0) {
            netd_hmac_sha256_update(&h, t, t_len);
        }
        if (info_len > 0 && info) {
            netd_hmac_sha256_update(&h, info, info_len);
        }
        netd_hmac_sha256_update(&h, &ctr, 1u);
        netd_hmac_sha256_final(&h, t);

        t_len = 32u;
        uint32_t take = out_len - produced;
        if (take > t_len) {
            take = t_len;
        }

        memcpy(dst + produced, t, take);
        produced += take;

        ctr++;
        if (ctr == 0) {
            memset(t, 0, sizeof(t));
            return 0;
        }
    }

    memset(t, 0, sizeof(t));
    return 1;
}

static uint32_t netd_str_len(const char* s) {
    if (!s) {
        return 0;
    }
    return (uint32_t)strlen(s);
}

int netd_hkdf_sha256_expand_label(
    const uint8_t prk[32],
    const char* label,
    const void* context,
    uint32_t context_len,
    void* out,
    uint32_t out_len
) {
    if (!prk || !label || (!out && out_len != 0)) {
        return 0;
    }

    const char* prefix = "tls13 ";
    uint32_t prefix_len = 6u;
    uint32_t label_len = netd_str_len(label);

    uint32_t full_label_len = prefix_len + label_len;
    if (full_label_len > 255u || context_len > 255u) {
        return 0;
    }

    uint8_t info[2 + 1 + 255 + 1 + 255];
    uint32_t w = 0;

    info[w + 0u] = (uint8_t)(out_len >> 8);
    info[w + 1u] = (uint8_t)(out_len);
    w += 2u;

    info[w] = (uint8_t)full_label_len;
    w += 1u;

    memcpy(info + w, prefix, prefix_len);
    w += prefix_len;
    memcpy(info + w, label, label_len);
    w += label_len;

    info[w] = (uint8_t)context_len;
    w += 1u;

    if (context_len > 0) {
        memcpy(info + w, context, context_len);
        w += context_len;
    }

    return netd_hkdf_sha256_expand(prk, info, w, out, out_len);
}

