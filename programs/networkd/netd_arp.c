// SPDX-License-Identifier: GPL-2.0

#include "netd_arp.h"

#include <yula.h>

#include "netd_config.h"
#include "netd_device.h"
#include "netd_iface.h"
#include "netd_proto.h"
#include "netd_util.h"

static int netd_arp_cache_lookup(netd_ctx_t* ctx, uint32_t ip, uint8_t out_mac[6]) {
    if (!ctx || !out_mac) {
        return 0;
    }

    for (uint32_t i = 0; i < NETD_ARP_CACHE_SIZE; i++) {
        if (!ctx->arp_cache[i].used) {
            continue;
        }
        if (ctx->arp_cache[i].ip != ip) {
            continue;
        }
        memcpy(out_mac, ctx->arp_cache[i].mac, 6);
        return 1;
    }

    return 0;
}

static void netd_arp_cache_update(netd_ctx_t* ctx, uint32_t ip, const uint8_t mac[6]) {
    if (!ctx || !mac) {
        return;
    }

    for (uint32_t i = 0; i < NETD_ARP_CACHE_SIZE; i++) {
        if (!ctx->arp_cache[i].used) {
            continue;
        }
        if (ctx->arp_cache[i].ip != ip) {
            continue;
        }
        memcpy(ctx->arp_cache[i].mac, mac, 6);
        return;
    }

    uint32_t slot = ctx->arp_next_slot % NETD_ARP_CACHE_SIZE;
    ctx->arp_next_slot++;

    ctx->arp_cache[slot].used = 1;
    ctx->arp_cache[slot].ip = ip;
    memcpy(ctx->arp_cache[slot].mac, mac, 6);
}

static int netd_send_arp_request(netd_ctx_t* ctx, uint32_t target_ip) {
    if (!ctx || !ctx->iface.up) {
        return -1;
    }

    net_eth_hdr_t* eth = (net_eth_hdr_t*)ctx->tx_buf;
    net_arp_t* arp = (net_arp_t*)(ctx->tx_buf + sizeof(net_eth_hdr_t));

    memset(eth->dst, 0xFF, 6);
    memcpy(eth->src, ctx->iface.mac, 6);
    eth->ethertype = netd_htons(0x0806u);

    arp->htype = netd_htons(1);
    arp->ptype = netd_htons(0x0800u);
    arp->hlen = 6;
    arp->plen = 4;
    arp->opcode = netd_htons(1);
    memcpy(arp->sha, ctx->iface.mac, 6);
    arp->spa = netd_htonl(ctx->iface.ip);
    memset(arp->tha, 0, 6);
    arp->tpa = netd_htonl(target_ip);

    uint32_t len = (uint32_t)(sizeof(net_eth_hdr_t) + sizeof(net_arp_t));
    return netd_iface_send_frame(ctx, ctx->tx_buf, len);
}

static int netd_send_arp_reply(netd_ctx_t* ctx, uint32_t target_ip, const uint8_t target_mac[6]) {
    if (!ctx || !ctx->iface.up || !target_mac) {
        return -1;
    }

    net_eth_hdr_t* eth = (net_eth_hdr_t*)ctx->tx_buf;
    net_arp_t* arp = (net_arp_t*)(ctx->tx_buf + sizeof(net_eth_hdr_t));

    memcpy(eth->dst, target_mac, 6);
    memcpy(eth->src, ctx->iface.mac, 6);
    eth->ethertype = netd_htons(0x0806u);

    arp->htype = netd_htons(1);
    arp->ptype = netd_htons(0x0800u);
    arp->hlen = 6;
    arp->plen = 4;
    arp->opcode = netd_htons(2);
    memcpy(arp->sha, ctx->iface.mac, 6);
    arp->spa = netd_htonl(ctx->iface.ip);
    memcpy(arp->tha, target_mac, 6);
    arp->tpa = netd_htonl(target_ip);

    uint32_t len = (uint32_t)(sizeof(net_eth_hdr_t) + sizeof(net_arp_t));
    return netd_iface_send_frame(ctx, ctx->tx_buf, len);
}

void netd_arp_init(netd_ctx_t* ctx) {
    if (!ctx) {
        return;
    }

    memset(ctx->arp_cache, 0, sizeof(ctx->arp_cache));
    ctx->arp_next_slot = 0;
}

void netd_arp_process_frame(netd_ctx_t* ctx, const uint8_t* buf, uint32_t len) {
    if (!ctx || !buf) {
        return;
    }

    if (len < sizeof(net_eth_hdr_t) + sizeof(net_arp_t)) {
        return;
    }

    const net_arp_t* arp = (const net_arp_t*)(buf + sizeof(net_eth_hdr_t));

    if (netd_ntohs(arp->htype) != 1) {
        return;
    }
    if (netd_ntohs(arp->ptype) != 0x0800u) {
        return;
    }
    if (arp->hlen != 6 || arp->plen != 4) {
        return;
    }

    uint32_t spa = netd_ntohl(arp->spa);
    uint32_t tpa = netd_ntohl(arp->tpa);

    netd_arp_cache_update(ctx, spa, arp->sha);

    if (netd_ntohs(arp->opcode) == 1 && tpa == ctx->iface.ip) {
        (void)netd_send_arp_reply(ctx, spa, arp->sha);
        return;
    }
}

int netd_arp_resolve_mac(netd_ctx_t* ctx, uint32_t target_ip, uint8_t out_mac[6], uint32_t timeout_ms) {
    if (!ctx || !ctx->iface.up || !out_mac) {
        return 0;
    }

    if (netd_arp_cache_lookup(ctx, target_ip, out_mac)) {
        return 1;
    }

    uint32_t elapsed = 0;
    uint32_t step_ms = 10;
    uint32_t next_send = 0;

    while (elapsed < timeout_ms) {
        if (elapsed >= next_send) {
            (void)netd_send_arp_request(ctx, target_ip);
            next_send += 200;
        }

        netd_device_process(ctx);

        if (netd_arp_cache_lookup(ctx, target_ip, out_mac)) {
            return 1;
        }

        sleep((int)step_ms);
        elapsed += step_ms;
    }

    return 0;
}

