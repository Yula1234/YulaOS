// SPDX-License-Identifier: GPL-2.0

#ifndef YOS_NETWORKD_X25519_H
#define YOS_NETWORKD_X25519_H

#include <stdint.h>

void netd_x25519(uint8_t out_shared[32], const uint8_t priv[32], const uint8_t peer_pub[32]);

void netd_x25519_public_key(uint8_t out_pub[32], const uint8_t priv[32]);

#endif
