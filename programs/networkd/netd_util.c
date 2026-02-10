// SPDX-License-Identifier: GPL-2.0

#include "netd_util.h"

#include <yula.h>

void netd_set_name(char dst[16], const char* src) {
    memset(dst, 0, 16);
    if (!src) {
        return;
    }

    size_t n = strlen(src);
    if (n > 15u) {
        n = 15u;
    }

    memcpy(dst, src, n);
}

static uint16_t netd_swap16(uint16_t v) {
    uint16_t hi = (uint16_t)(v << 8);
    uint16_t lo = (uint16_t)(v >> 8);
    return (uint16_t)(hi | lo);
}

static uint32_t netd_swap32(uint32_t v) {
    uint32_t b0 = (v & 0x000000FFu) << 24;
    uint32_t b1 = (v & 0x0000FF00u) << 8;
    uint32_t b2 = (v & 0x00FF0000u) >> 8;
    uint32_t b3 = (v & 0xFF000000u) >> 24;
    return b0 | b1 | b2 | b3;
}

uint16_t netd_htons(uint16_t v) {
    return netd_swap16(v);
}

uint16_t netd_ntohs(uint16_t v) {
    return netd_swap16(v);
}

uint32_t netd_htonl(uint32_t v) {
    return netd_swap32(v);
}

uint32_t netd_ntohl(uint32_t v) {
    return netd_swap32(v);
}

uint16_t netd_checksum16(const void* data, uint32_t len) {
    const uint8_t* buf = (const uint8_t*)data;
    uint32_t sum = 0;

    while (len > 1u) {
        sum += (uint32_t)((buf[0] << 8) | buf[1]);
        buf += 2;
        len -= 2;
    }

    if (len > 0) {
        sum += (uint32_t)(buf[0] << 8);
    }

    while (sum >> 16) {
        sum = (sum & 0xFFFFu) + (sum >> 16);
    }

    return (uint16_t)(~sum);
}

int netd_ip_same_subnet(uint32_t a, uint32_t b, uint32_t mask) {
    return (a & mask) == (b & mask);
}

