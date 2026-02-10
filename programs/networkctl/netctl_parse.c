// SPDX-License-Identifier: GPL-2.0

#include "netctl_common.h"

#include "netctl_parse.h"

int netctl_parse_u32(const char* s, uint32_t* out) {
    if (!s || !*s || !out) {
        return 0;
    }

    uint32_t v = 0;
    for (const char* p = s; *p; p++) {
        if (*p < '0' || *p > '9') {
            return 0;
        }

        uint32_t digit = (uint32_t)(*p - '0');
        if (v > (0xFFFFFFFFu - digit) / 10u) {
            return 0;
        }

        v = (v * 10u) + digit;
    }

    *out = v;
    return 1;
}

static int netctl_parse_ip4_octet(const char** io_p, uint32_t* out) {
    if (!io_p || !*io_p || !out) {
        return 0;
    }

    const char* p = *io_p;
    if (*p < '0' || *p > '9') {
        return 0;
    }

    uint32_t v = 0;
    uint32_t digits = 0;
    while (*p >= '0' && *p <= '9') {
        digits++;
        if (digits > 3) {
            return 0;
        }

        v = (v * 10u) + (uint32_t)(*p - '0');
        if (v > 255u) {
            return 0;
        }
        p++;
    }

    *io_p = p;
    *out = v;
    return 1;
}

int netctl_parse_ip4(const char* s, uint32_t* out) {
    if (!s || !*s || !out) {
        return 0;
    }

    const char* p = s;

    uint32_t a = 0;
    uint32_t b = 0;
    uint32_t c = 0;
    uint32_t d = 0;

    if (!netctl_parse_ip4_octet(&p, &a)) {
        return 0;
    }
    if (*p != '.') {
        return 0;
    }
    p++;

    if (!netctl_parse_ip4_octet(&p, &b)) {
        return 0;
    }
    if (*p != '.') {
        return 0;
    }
    p++;

    if (!netctl_parse_ip4_octet(&p, &c)) {
        return 0;
    }
    if (*p != '.') {
        return 0;
    }
    p++;

    if (!netctl_parse_ip4_octet(&p, &d)) {
        return 0;
    }
    if (*p != '\0') {
        return 0;
    }

    *out = (a << 24) | (b << 16) | (c << 8) | d;
    return 1;
}

