// SPDX-License-Identifier: GPL-2.0

#include "netd_device.h"

#include "netd_arp.h"
#include "netd_iface.h"
#include "netd_ipv4.h"
#include "netd_proto.h"
#include "netd_util.h"

static void netd_process_frame(netd_ctx_t* ctx, const uint8_t* buf, uint32_t len) {
    if (!ctx || !buf) {
        return;
    }

    if (len < sizeof(net_eth_hdr_t)) {
        return;
    }

    const net_eth_hdr_t* eth = (const net_eth_hdr_t*)buf;
    uint16_t ethertype = netd_ntohs(eth->ethertype);

    if (ethertype == 0x0806u) {
        netd_arp_process_frame(ctx, buf, len);
        return;
    }

    if (ethertype == 0x0800u) {
        netd_ipv4_process_frame(ctx, buf, len);
        return;
    }
}

void netd_device_process(netd_ctx_t* ctx) {
    if (!ctx) {
        return;
    }

    for (;;) {
        int r = netd_iface_read_frame(ctx, ctx->rx_buf, ctx->rx_buf_size);
        if (r <= 0) {
            break;
        }

        netd_process_frame(ctx, ctx->rx_buf, (uint32_t)r);
    }
}

