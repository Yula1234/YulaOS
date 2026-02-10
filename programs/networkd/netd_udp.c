// SPDX-License-Identifier: GPL-2.0

#include "netd_udp.h"

#include <yula.h>

#include "netd_arp.h"
#include "netd_config.h"
#include "netd_iface.h"
#include "netd_proto.h"
#include "netd_util.h"

int netd_udp_send(netd_ctx_t* ctx, uint32_t dst_ip, uint16_t dst_port, uint16_t src_port, const uint8_t* payload, uint32_t payload_len) {
    if (!ctx || !ctx->iface.up || !payload) {
        return 0;
    }

    uint32_t ip_payload_len = (uint32_t)sizeof(net_udp_hdr_t) + payload_len;
    uint32_t ip_total_len = (uint32_t)sizeof(net_ipv4_hdr_t) + ip_payload_len;
    uint32_t frame_len = (uint32_t)sizeof(net_eth_hdr_t) + ip_total_len;

    if (frame_len > (uint32_t)sizeof(ctx->tx_buf)) {
        return 0;
    }

    uint32_t next_hop = netd_iface_next_hop_ip(ctx, dst_ip);
    if (next_hop == 0) {
        return 0;
    }

    uint8_t dst_mac[6];
    if (!netd_arp_resolve_mac(ctx, next_hop, dst_mac, NETD_ARP_TIMEOUT_MS)) {
        return 0;
    }

    net_eth_hdr_t* eth = (net_eth_hdr_t*)ctx->tx_buf;
    net_ipv4_hdr_t* ip = (net_ipv4_hdr_t*)(ctx->tx_buf + sizeof(net_eth_hdr_t));
    net_udp_hdr_t* udp = (net_udp_hdr_t*)((uint8_t*)ip + sizeof(net_ipv4_hdr_t));

    memcpy(eth->dst, dst_mac, 6);
    memcpy(eth->src, ctx->iface.mac, 6);
    eth->ethertype = netd_htons(0x0800u);

    ip->ver_ihl = 0x45u;
    ip->tos = 0;
    ip->total_len = netd_htons((uint16_t)ip_total_len);
    ip->id = 0;
    ip->flags_frag = 0;
    ip->ttl = 64;
    ip->proto = 17;
    ip->src = netd_htonl(ctx->iface.ip);
    ip->dst = netd_htonl(dst_ip);
    ip->hdr_checksum = 0;
    ip->hdr_checksum = netd_htons(netd_checksum16(ip, sizeof(net_ipv4_hdr_t)));

    udp->src_port = netd_htons(src_port);
    udp->dst_port = netd_htons(dst_port);
    udp->len = netd_htons((uint16_t)ip_payload_len);
    udp->checksum = 0;

    uint8_t* out = (uint8_t*)udp + sizeof(net_udp_hdr_t);
    memcpy(out, payload, payload_len);

    return netd_iface_send_frame(ctx, ctx->tx_buf, frame_len) > 0;
}

