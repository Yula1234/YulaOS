// SPDX-License-Identifier: GPL-2.0

#include "netd_ipv4.h"

#include <yula.h>

#include "netd_arp.h"
#include "netd_config.h"
#include "netd_device.h"
#include "netd_dns.h"
#include "netd_iface.h"
#include "netd_proto.h"
#include "netd_util.h"

static void netd_send_icmp_reply(netd_ctx_t* ctx, const net_eth_hdr_t* rx_eth, const net_ipv4_hdr_t* rx_ip, const net_icmp_hdr_t* rx_icmp, uint32_t icmp_len) {
    if (!ctx || !rx_eth || !rx_ip || !rx_icmp) {
        return;
    }

    net_eth_hdr_t* eth = (net_eth_hdr_t*)ctx->tx_buf;
    net_ipv4_hdr_t* ip = (net_ipv4_hdr_t*)(ctx->tx_buf + sizeof(net_eth_hdr_t));
    net_icmp_hdr_t* icmp = (net_icmp_hdr_t*)((uint8_t*)ip + sizeof(net_ipv4_hdr_t));

    memcpy(eth->dst, rx_eth->src, 6);
    memcpy(eth->src, ctx->iface.mac, 6);
    eth->ethertype = netd_htons(0x0800u);

    ip->ver_ihl = 0x45u;
    ip->tos = 0;
    ip->total_len = netd_htons((uint16_t)(sizeof(net_ipv4_hdr_t) + icmp_len));
    ip->id = 0;
    ip->flags_frag = 0;
    ip->ttl = 64;
    ip->proto = 1;
    ip->src = rx_ip->dst;
    ip->dst = rx_ip->src;
    ip->hdr_checksum = 0;
    ip->hdr_checksum = netd_htons(netd_checksum16(ip, sizeof(net_ipv4_hdr_t)));

    memcpy(icmp, rx_icmp, icmp_len);
    icmp->type = 0;
    icmp->code = 0;
    icmp->checksum = 0;
    icmp->checksum = netd_htons(netd_checksum16(icmp, icmp_len));

    uint32_t total_len = (uint32_t)(sizeof(net_eth_hdr_t) + sizeof(net_ipv4_hdr_t) + icmp_len);
    (void)netd_iface_send_frame(ctx, ctx->tx_buf, total_len);
}

static int netd_send_icmp_echo(netd_ctx_t* ctx, uint32_t dst_ip, uint16_t seq, const uint8_t dst_mac[6]) {
    if (!ctx || !ctx->iface.up || !dst_mac) {
        return 0;
    }

    net_eth_hdr_t* eth = (net_eth_hdr_t*)ctx->tx_buf;
    net_ipv4_hdr_t* ip = (net_ipv4_hdr_t*)(ctx->tx_buf + sizeof(net_eth_hdr_t));
    net_icmp_hdr_t* icmp = (net_icmp_hdr_t*)((uint8_t*)ip + sizeof(net_ipv4_hdr_t));
    uint8_t* data = (uint8_t*)icmp + sizeof(net_icmp_hdr_t);

    memcpy(eth->dst, dst_mac, 6);
    memcpy(eth->src, ctx->iface.mac, 6);
    eth->ethertype = netd_htons(0x0800u);

    ip->ver_ihl = 0x45u;
    ip->tos = 0;
    ip->total_len = netd_htons((uint16_t)(sizeof(net_ipv4_hdr_t) + sizeof(net_icmp_hdr_t) + NETD_ICMP_DATA_SIZE));
    ip->id = 0;
    ip->flags_frag = 0;
    ip->ttl = 64;
    ip->proto = 1;
    ip->src = netd_htonl(ctx->iface.ip);
    ip->dst = netd_htonl(dst_ip);
    ip->hdr_checksum = 0;
    ip->hdr_checksum = netd_htons(netd_checksum16(ip, sizeof(net_ipv4_hdr_t)));

    icmp->type = 8;
    icmp->code = 0;
    icmp->checksum = 0;
    icmp->id = netd_htons(NETD_PING_ID);
    icmp->seq = netd_htons(seq);

    for (uint32_t i = 0; i < NETD_ICMP_DATA_SIZE; i++) {
        data[i] = (uint8_t)(i & 0xFFu);
    }

    icmp->checksum = netd_htons(netd_checksum16(icmp, (uint32_t)(sizeof(net_icmp_hdr_t) + NETD_ICMP_DATA_SIZE)));

    uint32_t total_len = (uint32_t)(sizeof(net_eth_hdr_t) + sizeof(net_ipv4_hdr_t) + sizeof(net_icmp_hdr_t) + NETD_ICMP_DATA_SIZE);
    return netd_iface_send_frame(ctx, ctx->tx_buf, total_len) > 0;
}

static int netd_wait_for_ping(netd_ctx_t* ctx, uint32_t dst_ip, uint16_t seq, uint32_t timeout_ms, uint32_t* out_rtt) {
    if (!ctx) {
        return 0;
    }

    ctx->ping_wait.active = 1;
    ctx->ping_wait.received = 0;
    ctx->ping_wait.id = NETD_PING_ID;
    ctx->ping_wait.seq = seq;
    ctx->ping_wait.target_ip = dst_ip;

    uint32_t elapsed = 0;
    uint32_t step_ms = 10;

    while (elapsed < timeout_ms) {
        netd_device_process(ctx);

        if (ctx->ping_wait.received) {
            ctx->ping_wait.active = 0;
            if (out_rtt) {
                *out_rtt = elapsed;
            }
            return 1;
        }

        sleep((int)step_ms);
        elapsed += step_ms;
    }

    ctx->ping_wait.active = 0;
    return 0;
}

void netd_ipv4_process_frame(netd_ctx_t* ctx, const uint8_t* buf, uint32_t len) {
    if (!ctx || !buf) {
        return;
    }

    if (len < sizeof(net_eth_hdr_t) + sizeof(net_ipv4_hdr_t)) {
        return;
    }

    const net_eth_hdr_t* eth = (const net_eth_hdr_t*)buf;
    const net_ipv4_hdr_t* ip = (const net_ipv4_hdr_t*)(buf + sizeof(net_eth_hdr_t));

    uint32_t ihl = (uint32_t)(ip->ver_ihl & 0x0Fu) * 4u;
    if ((ip->ver_ihl >> 4) != 4) {
        return;
    }
    if (ihl < sizeof(net_ipv4_hdr_t)) {
        return;
    }
    if (len < sizeof(net_eth_hdr_t) + ihl) {
        return;
    }
    if (netd_checksum16(ip, ihl) != 0) {
        return;
    }

    uint16_t total_len = netd_ntohs(ip->total_len);
    if (total_len < ihl) {
        return;
    }
    if ((uint32_t)total_len > len - sizeof(net_eth_hdr_t)) {
        return;
    }

    uint32_t dst = netd_ntohl(ip->dst);
    if (dst != ctx->iface.ip && dst != 0xFFFFFFFFu) {
        return;
    }

    const uint8_t* payload = (const uint8_t*)ip + ihl;
    uint32_t payload_len = (uint32_t)total_len - ihl;

    if (ip->proto == 1) {
        if (payload_len < sizeof(net_icmp_hdr_t)) {
            return;
        }

        const net_icmp_hdr_t* icmp = (const net_icmp_hdr_t*)payload;
        uint32_t icmp_len = payload_len;

        if (netd_checksum16(icmp, icmp_len) != 0) {
            return;
        }

        if (icmp->type == 8 && dst == ctx->iface.ip) {
            netd_send_icmp_reply(ctx, eth, ip, icmp, icmp_len);
            return;
        }

        if (icmp->type == 0) {
            if (ctx->ping_wait.active && !ctx->ping_wait.received) {
                if (icmp->id == netd_htons(ctx->ping_wait.id) && icmp->seq == netd_htons(ctx->ping_wait.seq)) {
                    uint32_t src = netd_ntohl(ip->src);
                    if (src == ctx->ping_wait.target_ip) {
                        ctx->ping_wait.received = 1;
                    }
                }
            }
        }
        return;
    }

    if (ip->proto == 17) {
        netd_dns_process_udp(ctx, ip, payload, payload_len);
        return;
    }
}

uint32_t netd_ipv4_send_ping(netd_ctx_t* ctx, uint32_t dst_ip, uint32_t timeout_ms, uint16_t seq, uint32_t* out_rtt) {
    if (!ctx) {
        return NET_STATUS_UNREACHABLE;
    }

    if (netd_iface_ensure_up(ctx) != 0) {
        return NET_STATUS_UNREACHABLE;
    }

    uint32_t next_hop = dst_ip;
    if (!netd_ip_same_subnet(dst_ip, ctx->iface.ip, ctx->iface.mask)) {
        if (ctx->iface.gw == 0) {
            return NET_STATUS_UNREACHABLE;
        }
        next_hop = ctx->iface.gw;
    }

    uint8_t dst_mac[6];
    if (!netd_arp_resolve_mac(ctx, next_hop, dst_mac, NETD_ARP_TIMEOUT_MS)) {
        return NET_STATUS_TIMEOUT;
    }

    if (!netd_send_icmp_echo(ctx, dst_ip, seq, dst_mac)) {
        return NET_STATUS_TIMEOUT;
    }

    if (netd_wait_for_ping(ctx, dst_ip, seq, timeout_ms, out_rtt)) {
        return NET_STATUS_OK;
    }

    return NET_STATUS_TIMEOUT;
}

