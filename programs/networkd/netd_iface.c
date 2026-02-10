// SPDX-License-Identifier: GPL-2.0

#include "netd_iface.h"

#include <yula.h>

#include "netd_util.h"

int netd_iface_init(netd_ctx_t* ctx) {
    if (!ctx) {
        return -1;
    }

    memset(&ctx->iface, 0, sizeof(ctx->iface));

    ctx->iface.fd = open("/dev/ne2k0", 0);
    if (ctx->iface.fd < 0) {
        ctx->iface.fd = -1;
        return -1;
    }

    yos_net_mac_t mac;
    if (ioctl(ctx->iface.fd, YOS_NET_GET_MAC, &mac) == 0) {
        memcpy(ctx->iface.mac, mac.mac, sizeof(mac.mac));
    } else {
        close(ctx->iface.fd);
        ctx->iface.fd = -1;
        return -1;
    }

    ctx->iface.ip = NETD_DEFAULT_IP;
    ctx->iface.mask = NETD_DEFAULT_MASK;
    ctx->iface.gw = NETD_DEFAULT_GW;
    ctx->iface.up = 1;
    if (ctx->dns_server == 0) {
        ctx->dns_server = NETD_DEFAULT_DNS;
    }
    return 0;
}

void netd_iface_close(netd_ctx_t* ctx) {
    if (!ctx) {
        return;
    }

    if (ctx->iface.fd >= 0) {
        close(ctx->iface.fd);
    }

    memset(&ctx->iface, 0, sizeof(ctx->iface));
    ctx->iface.fd = -1;
}

int netd_iface_ensure_up(netd_ctx_t* ctx) {
    if (!ctx) {
        return -1;
    }

    if (ctx->iface.up && ctx->iface.fd >= 0) {
        return 0;
    }

    netd_iface_close(ctx);

    if (netd_iface_init(ctx) != 0) {
        return -1;
    }

    return 0;
}

void netd_iface_periodic(netd_ctx_t* ctx) {
    if (!ctx) {
        return;
    }

    uint32_t now_ms = uptime_ms();

    if (ctx->iface.up && ctx->iface.fd >= 0) {
        ctx->iface_last_try_ms = now_ms;
        return;
    }

    if ((now_ms - ctx->iface_last_try_ms) < 1000u) {
        return;
    }

    ctx->iface_last_try_ms = now_ms;

    if (netd_iface_ensure_up(ctx) == 0) {
        netd_links_init(ctx);
        netd_iface_print_state(ctx);
    }
}

void netd_iface_print_state(const netd_ctx_t* ctx) {
    if (!ctx || !ctx->iface.up || ctx->iface.fd < 0) {
        printf("networkd: iface down (/dev/ne2k0 unavailable)\n");
        return;
    }

    printf("networkd: iface ne2k0 up\n");
    printf(
        "networkd: mac %02X:%02X:%02X:%02X:%02X:%02X\n",
        (unsigned)ctx->iface.mac[0],
        (unsigned)ctx->iface.mac[1],
        (unsigned)ctx->iface.mac[2],
        (unsigned)ctx->iface.mac[3],
        (unsigned)ctx->iface.mac[4],
        (unsigned)ctx->iface.mac[5]
    );

    printf(
        "networkd: ip %u.%u.%u.%u mask %u.%u.%u.%u gw %u.%u.%u.%u\n",
        (unsigned)((ctx->iface.ip >> 24) & 0xFFu),
        (unsigned)((ctx->iface.ip >> 16) & 0xFFu),
        (unsigned)((ctx->iface.ip >> 8) & 0xFFu),
        (unsigned)(ctx->iface.ip & 0xFFu),
        (unsigned)((ctx->iface.mask >> 24) & 0xFFu),
        (unsigned)((ctx->iface.mask >> 16) & 0xFFu),
        (unsigned)((ctx->iface.mask >> 8) & 0xFFu),
        (unsigned)(ctx->iface.mask & 0xFFu),
        (unsigned)((ctx->iface.gw >> 24) & 0xFFu),
        (unsigned)((ctx->iface.gw >> 16) & 0xFFu),
        (unsigned)((ctx->iface.gw >> 8) & 0xFFu),
        (unsigned)(ctx->iface.gw & 0xFFu)
    );

    printf(
        "networkd: dns %u.%u.%u.%u\n",
        (unsigned)((ctx->dns_server >> 24) & 0xFFu),
        (unsigned)((ctx->dns_server >> 16) & 0xFFu),
        (unsigned)((ctx->dns_server >> 8) & 0xFFu),
        (unsigned)(ctx->dns_server & 0xFFu)
    );
}

void netd_links_init(netd_ctx_t* ctx) {
    if (!ctx) {
        return;
    }

    memset(&ctx->state, 0, sizeof(ctx->state));

    net_link_info_t lo;
    memset(&lo, 0, sizeof(lo));
    netd_set_name(lo.name, "lo");
    lo.flags = NET_LINK_FLAG_PRESENT | NET_LINK_FLAG_UP | NET_LINK_FLAG_LOOPBACK;
    lo.ipv4_addr = 0x7F000001u;
    lo.ipv4_mask = 0xFF000000u;
    ctx->state.links[ctx->state.count++] = lo;

    net_link_info_t ne2k;
    memset(&ne2k, 0, sizeof(ne2k));
    netd_set_name(ne2k.name, "ne2k0");
    ne2k.flags = NET_LINK_FLAG_PRESENT;
    if (ctx->iface.up) {
        ne2k.flags |= NET_LINK_FLAG_UP;
        ne2k.ipv4_addr = ctx->iface.ip;
        ne2k.ipv4_mask = ctx->iface.mask;
    }
    ctx->state.links[ctx->state.count++] = ne2k;
}

uint32_t netd_iface_next_hop_ip(const netd_ctx_t* ctx, uint32_t dst_ip) {
    if (!ctx) {
        return 0;
    }

    if (netd_ip_same_subnet(dst_ip, ctx->iface.ip, ctx->iface.mask)) {
        return dst_ip;
    }

    return ctx->iface.gw;
}

int netd_iface_read_frame(netd_ctx_t* ctx, uint8_t* buf, uint32_t cap) {
    if (!ctx || ctx->iface.fd < 0) {
        return -1;
    }
    return read(ctx->iface.fd, buf, cap);
}

int netd_iface_send_frame(netd_ctx_t* ctx, const uint8_t* buf, uint32_t len) {
    if (!ctx || ctx->iface.fd < 0) {
        return -1;
    }
    return write(ctx->iface.fd, buf, len);
}
