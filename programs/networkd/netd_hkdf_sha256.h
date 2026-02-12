// SPDX-License-Identifier: GPL-2.0

#ifndef YOS_NETWORKD_HKDF_SHA256_H
#define YOS_NETWORKD_HKDF_SHA256_H

#include <stdint.h>

void netd_hkdf_sha256_extract(const void* salt, uint32_t salt_len, const void* ikm, uint32_t ikm_len, uint8_t out_prk[32]);

int netd_hkdf_sha256_expand(const uint8_t prk[32], const void* info, uint32_t info_len, void* out, uint32_t out_len);

int netd_hkdf_sha256_expand_label(
    const uint8_t prk[32],
    const char* label,
    const void* context,
    uint32_t context_len,
    void* out,
    uint32_t out_len
);

#endif
