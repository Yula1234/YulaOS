// SPDX-License-Identifier: GPL-2.0

#include "netctl_common.h"

#include "netctl_fmt.h"
#include "netctl_print.h"

void netctl_print_links(const uint8_t* payload, uint32_t len) {
    if (!payload || len < (uint32_t)sizeof(net_link_list_hdr_t)) {
        printf("links: invalid response\n");
        return;
    }

    net_link_list_hdr_t hdr;
    memcpy(&hdr, payload, sizeof(hdr));

    uint32_t expected =
        (uint32_t)sizeof(net_link_list_hdr_t) +
        (hdr.count * (uint32_t)sizeof(net_link_info_t));
    if (len < expected) {
        printf("links: truncated response\n");
        return;
    }

    const uint8_t* ptr = payload + sizeof(net_link_list_hdr_t);
    for (uint32_t i = 0; i < hdr.count; i++) {
        net_link_info_t info;
        memcpy(&info, ptr, sizeof(info));
        ptr += sizeof(info);

        char ip_str[32];
        char mask_str[32];
        char mac_str[32];
        netctl_ip4_to_str(info.ipv4_addr, ip_str, (uint32_t)sizeof(ip_str));
        netctl_ip4_to_str(info.ipv4_mask, mask_str, (uint32_t)sizeof(mask_str));
        netctl_mac_to_str(info.mac, mac_str, (uint32_t)sizeof(mac_str));

        const char* state = (info.flags & NET_LINK_FLAG_UP) ? "up" : "down";
        const char* kind = (info.flags & NET_LINK_FLAG_LOOPBACK) ? "loopback" : "ethernet";
        printf("%s  %s  %s  %s/%s  %s\n", info.name, kind, state, ip_str, mask_str, mac_str);
    }
}

void netctl_print_cfg(const net_cfg_resp_t* cfg) {
    if (!cfg) {
        return;
    }

    char ip[32];
    char mask[32];
    char gw[32];
    char dns[32];

    netctl_ip4_to_str(cfg->ip, ip, (uint32_t)sizeof(ip));
    netctl_ip4_to_str(cfg->mask, mask, (uint32_t)sizeof(mask));
    netctl_ip4_to_str(cfg->gw, gw, (uint32_t)sizeof(gw));
    netctl_ip4_to_str(cfg->dns, dns, (uint32_t)sizeof(dns));

    printf("config:\n");
    printf("  ip:   %s\n", ip);
    printf("  mask: %s\n", mask);
    printf("  gw:   %s\n", gw);
    printf("  dns:  %s\n", dns);
}

void netctl_print_usage(void) {
    printf("networkctl - network manager control tool\n\n");
    printf("usage:\n");
    printf("  networkctl\n");
    printf("  networkctl status\n");
    printf("  networkctl links\n");
    printf("  networkctl ping <ip|name> [-c count] [-t timeout_ms]\n");
    printf("  networkctl resolve <name> [-t timeout_ms]\n");
    printf("  networkctl config show\n");
    printf("  networkctl config set [ip A.B.C.D] [mask A.B.C.D] [gw A.B.C.D] [dns A.B.C.D]\n");
    printf("  networkctl up\n");
    printf("  networkctl down\n");
    printf("  networkctl daemon status\n");
    printf("  networkctl daemon start\n");
    printf("  networkctl daemon stop\n");
    printf("  networkctl daemon restart\n");
}

