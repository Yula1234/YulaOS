// SPDX-License-Identifier: GPL-2.0

#include "netctl_common.h"

#include "netctl_fmt.h"

void netctl_ip4_to_str(uint32_t addr, char* out, uint32_t cap) {
    if (!out || cap == 0) {
        return;
    }

    uint8_t a = (uint8_t)((addr >> 24) & 0xFFu);
    uint8_t b = (uint8_t)((addr >> 16) & 0xFFu);
    uint8_t c = (uint8_t)((addr >> 8) & 0xFFu);
    uint8_t d = (uint8_t)(addr & 0xFFu);

    snprintf(out, cap, "%u.%u.%u.%u", a, b, c, d);
}

void netctl_mac_to_str(const uint8_t mac[6], char* out, uint32_t cap) {
    if (!out || cap == 0 || !mac) {
        return;
    }

    snprintf(
        out,
        cap,
        "%02x:%02x:%02x:%02x:%02x:%02x",
        mac[0],
        mac[1],
        mac[2],
        mac[3],
        mac[4],
        mac[5]
    );
}

