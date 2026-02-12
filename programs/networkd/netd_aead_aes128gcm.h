// SPDX-License-Identifier: GPL-2.0

#ifndef YOS_NETWORKD_AEAD_AES128GCM_H
#define YOS_NETWORKD_AEAD_AES128GCM_H

#include <stdint.h>

int netd_aead_aes128gcm_seal(
    const uint8_t key[16],
    const uint8_t nonce[12],
    const void* aad,
    uint32_t aad_len,
    const void* plaintext,
    uint32_t plaintext_len,
    void* out_ciphertext,
    uint8_t out_tag[16]
);

int netd_aead_aes128gcm_open(
    const uint8_t key[16],
    const uint8_t nonce[12],
    const void* aad,
    uint32_t aad_len,
    const void* ciphertext,
    uint32_t ciphertext_len,
    const uint8_t tag[16],
    void* out_plaintext
);

#endif
