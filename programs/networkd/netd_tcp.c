// SPDX-License-Identifier: GPL-2.0

#include "netd_tcp.h"

#include <yula.h>

#include "netd_arp.h"
#include "netd_device.h"
#include "netd_iface.h"
#include "netd_util.h"

#define NETD_TCP_PROTO 6u

#define NETD_TCP_FLAG_FIN 0x01u
#define NETD_TCP_FLAG_SYN 0x02u
#define NETD_TCP_FLAG_RST 0x04u
#define NETD_TCP_FLAG_PSH 0x08u
#define NETD_TCP_FLAG_ACK 0x10u

#define NETD_TCP_STATE_CLOSED 0u
#define NETD_TCP_STATE_SYN_SENT 1u
#define NETD_TCP_STATE_ESTABLISHED 2u
#define NETD_TCP_STATE_FIN_WAIT_1 3u
#define NETD_TCP_STATE_FIN_WAIT_2 4u
#define NETD_TCP_STATE_CLOSE_WAIT 5u
#define NETD_TCP_STATE_LAST_ACK 6u

#define NETD_TCP_MSS 1200u

static uint32_t netd_tcp_rx_count(const netd_tcp_conn_t* c) {
    if (!c) {
        return 0;
    }

    if (c->rx_w >= c->rx_r) {
        return c->rx_w - c->rx_r;
    }

    return (uint32_t)NETD_TCP_RX_CAP - (c->rx_r - c->rx_w);
}

static uint32_t netd_tcp_rx_space(const netd_tcp_conn_t* c) {
    uint32_t used = netd_tcp_rx_count(c);
    if (used >= (uint32_t)NETD_TCP_RX_CAP - 1u) {
        return 0;
    }
    return ((uint32_t)NETD_TCP_RX_CAP - 1u) - used;
}

static uint16_t netd_tcp_window(const netd_tcp_conn_t* c) {
    uint32_t space = netd_tcp_rx_space(c);
    if (space > 0xFFFFu) {
        space = 0xFFFFu;
    }
    return (uint16_t)space;
}

static uint32_t netd_tcp_rx_write(netd_tcp_conn_t* c, const uint8_t* data, uint32_t len) {
    if (!c || !data || len == 0) {
        return 0;
    }

    uint32_t space = netd_tcp_rx_space(c);
    if (len > space) {
        len = space;
    }

    if (len == 0) {
        return 0;
    }

    uint32_t wi = c->rx_w;
    uint32_t first = (uint32_t)NETD_TCP_RX_CAP - wi;
    if (first > len) {
        first = len;
    }

    memcpy(c->rx_buf + wi, data, first);

    if (len > first) {
        memcpy(c->rx_buf, data + first, len - first);
    }

    wi += len;
    if (wi >= (uint32_t)NETD_TCP_RX_CAP) {
        wi -= (uint32_t)NETD_TCP_RX_CAP;
    }
    c->rx_w = wi;

    return len;
}

static uint32_t netd_tcp_rx_read(netd_tcp_conn_t* c, uint8_t* out, uint32_t cap) {
    if (!c || !out || cap == 0) {
        return 0;
    }

    uint32_t avail = netd_tcp_rx_count(c);
    if (cap > avail) {
        cap = avail;
    }

    if (cap == 0) {
        return 0;
    }

    uint32_t ri = c->rx_r;
    uint32_t first = (uint32_t)NETD_TCP_RX_CAP - ri;
    if (first > cap) {
        first = cap;
    }

    memcpy(out, c->rx_buf + ri, first);

    if (cap > first) {
        memcpy(out + first, c->rx_buf, cap - first);
    }

    ri += cap;
    if (ri >= (uint32_t)NETD_TCP_RX_CAP) {
        ri -= (uint32_t)NETD_TCP_RX_CAP;
    }
    c->rx_r = ri;

    return cap;
}

static uint32_t netd_tcp_sum16_add(uint32_t sum, uint16_t v) {
    sum += (uint32_t)v;
    return sum;
}

static uint32_t netd_tcp_sum16_buf(uint32_t sum, const uint8_t* buf, uint32_t len) {
    if (!buf) {
        return sum;
    }

    uint32_t i = 0;
    while (i + 1u < len) {
        uint16_t w = (uint16_t)((uint16_t)buf[i] << 8) | (uint16_t)buf[i + 1u];
        sum = netd_tcp_sum16_add(sum, w);
        i += 2u;
    }

    if (i < len) {
        uint16_t w = (uint16_t)((uint16_t)buf[i] << 8);
        sum = netd_tcp_sum16_add(sum, w);
    }

    return sum;
}

static uint16_t netd_tcp_sum16_finalize(uint32_t sum) {
    while ((sum >> 16) != 0u) {
        sum = (sum & 0xFFFFu) + (sum >> 16);
    }
    return (uint16_t)(~sum & 0xFFFFu);
}

static uint16_t netd_tcp_checksum(
    const net_ipv4_hdr_t* ip,
    const uint8_t* tcp_hdr,
    uint32_t tcp_hdr_len,
    const uint8_t* payload,
    uint32_t payload_len
) {
    if (!ip || !tcp_hdr) {
        return 0xFFFFu;
    }

    if (tcp_hdr_len < (uint32_t)sizeof(net_tcp_hdr_t)) {
        return 0xFFFFu;
    }

    uint32_t tcp_len = tcp_hdr_len + payload_len;
    if (tcp_len > 0xFFFFu) {
        return 0xFFFFu;
    }

    uint32_t sum = 0;
    sum = netd_tcp_sum16_buf(sum, (const uint8_t*)&ip->src, 4);
    sum = netd_tcp_sum16_buf(sum, (const uint8_t*)&ip->dst, 4);
    sum = netd_tcp_sum16_add(sum, (uint16_t)NETD_TCP_PROTO);
    sum = netd_tcp_sum16_add(sum, (uint16_t)tcp_len);

    sum = netd_tcp_sum16_buf(sum, tcp_hdr, tcp_hdr_len);
    sum = netd_tcp_sum16_buf(sum, payload, payload_len);

    return netd_tcp_sum16_finalize(sum);
}

static void netd_tcp_reset(netd_ctx_t* ctx) {
    if (!ctx) {
        return;
    }

    memset(&ctx->tcp, 0, sizeof(ctx->tcp));
}

static int netd_tcp_send_segment(netd_ctx_t* ctx, uint8_t flags, const uint8_t* payload, uint32_t payload_len) {
    if (!ctx || !ctx->iface.up) {
        return 0;
    }
    if (!ctx->tcp.active) {
        return 0;
    }
    if (payload_len > 0 && !payload) {
        return 0;
    }

    uint32_t ip_payload_len = (uint32_t)sizeof(net_tcp_hdr_t) + payload_len;
    uint32_t ip_total_len = (uint32_t)sizeof(net_ipv4_hdr_t) + ip_payload_len;
    uint32_t frame_len = (uint32_t)sizeof(net_eth_hdr_t) + ip_total_len;

    if (frame_len > (uint32_t)sizeof(ctx->tx_buf)) {
        return 0;
    }

    uint32_t next_hop = netd_iface_next_hop_ip(ctx, ctx->tcp.remote_ip);
    if (next_hop == 0) {
        return 0;
    }

    uint8_t dst_mac[6];
    if (!netd_arp_resolve_mac(ctx, next_hop, dst_mac, NETD_ARP_TIMEOUT_MS)) {
        return 0;
    }

    net_eth_hdr_t* eth = (net_eth_hdr_t*)ctx->tx_buf;
    net_ipv4_hdr_t* ip = (net_ipv4_hdr_t*)(ctx->tx_buf + sizeof(net_eth_hdr_t));
    net_tcp_hdr_t* tcp = (net_tcp_hdr_t*)((uint8_t*)ip + sizeof(net_ipv4_hdr_t));
    uint8_t* out = (uint8_t*)tcp + sizeof(net_tcp_hdr_t);

    memcpy(eth->dst, dst_mac, 6);
    memcpy(eth->src, ctx->iface.mac, 6);
    eth->ethertype = netd_htons(0x0800u);

    ip->ver_ihl = 0x45u;
    ip->tos = 0;
    ip->total_len = netd_htons((uint16_t)ip_total_len);
    ip->id = 0;
    ip->flags_frag = 0;
    ip->ttl = 64;
    ip->proto = NETD_TCP_PROTO;
    ip->src = netd_htonl(ctx->iface.ip);
    ip->dst = netd_htonl(ctx->tcp.remote_ip);
    ip->hdr_checksum = 0;
    ip->hdr_checksum = netd_htons(netd_checksum16(ip, sizeof(net_ipv4_hdr_t)));

    tcp->src_port = netd_htons(ctx->tcp.local_port);
    tcp->dst_port = netd_htons(ctx->tcp.remote_port);
    tcp->seq = netd_htonl(ctx->tcp.snd_nxt);
    tcp->ack = netd_htonl(ctx->tcp.rcv_nxt);
    tcp->data_offset = (uint8_t)(5u << 4);
    tcp->flags = flags;
    tcp->window = netd_htons(netd_tcp_window(&ctx->tcp));
    tcp->checksum = 0;
    tcp->urg_ptr = 0;

    if (payload_len > 0) {
        memcpy(out, payload, payload_len);
    }

    uint16_t csum = netd_tcp_checksum(ip, (const uint8_t*)tcp, (uint32_t)sizeof(net_tcp_hdr_t), payload, payload_len);
    tcp->checksum = netd_htons(csum);

    int sent = netd_iface_send_frame(ctx, ctx->tx_buf, frame_len) > 0;
    if (sent) {
        ctx->tcp.last_activity_ms = uptime_ms();
    }
    return sent;
}

static void netd_tcp_send_ack(netd_ctx_t* ctx) {
    if (!ctx || !ctx->tcp.active) {
        return;
    }
    (void)netd_tcp_send_segment(ctx, NETD_TCP_FLAG_ACK, 0, 0);
}

void netd_tcp_init(netd_ctx_t* ctx) {
    netd_tcp_reset(ctx);
}

void netd_tcp_process_ipv4(netd_ctx_t* ctx, const net_ipv4_hdr_t* ip, const uint8_t* payload, uint32_t payload_len) {
    if (!ctx || !ip || !payload) {
        return;
    }

    if (payload_len < (uint32_t)sizeof(net_tcp_hdr_t)) {
        return;
    }

    const net_tcp_hdr_t* tcp = (const net_tcp_hdr_t*)payload;
    uint32_t data_offset = (uint32_t)(tcp->data_offset >> 4) * 4u;
    if (data_offset < (uint32_t)sizeof(net_tcp_hdr_t)) {
        return;
    }
    if (data_offset > payload_len) {
        return;
    }

    uint32_t seg_data_len = payload_len - data_offset;
    const uint8_t* seg_data = payload + data_offset;

    uint16_t csum = netd_tcp_checksum(ip, payload, data_offset, seg_data, seg_data_len);
    if (csum != 0) {
        return;
    }

    uint16_t src_port = netd_ntohs(tcp->src_port);
    uint16_t dst_port = netd_ntohs(tcp->dst_port);
    uint32_t src_ip = netd_ntohl(ip->src);

    if (!ctx->tcp.active) {
        return;
    }

    if (src_ip != ctx->tcp.remote_ip || src_port != ctx->tcp.remote_port || dst_port != ctx->tcp.local_port) {
        return;
    }

    uint8_t flags = tcp->flags;
    uint32_t seq = netd_ntohl(tcp->seq);
    uint32_t ack = netd_ntohl(tcp->ack);

    if ((flags & NETD_TCP_FLAG_RST) != 0u) {
        netd_tcp_reset(ctx);
        return;
    }

    if (ack > ctx->tcp.snd_una && ack <= ctx->tcp.snd_nxt) {
        ctx->tcp.snd_una = ack;
        if (ctx->tcp.fin_sent && ctx->tcp.snd_una == ctx->tcp.snd_nxt) {
            ctx->tcp.fin_acked = 1;
        }
    }

    if (ctx->tcp.state == NETD_TCP_STATE_SYN_SENT) {
        if (((flags & NETD_TCP_FLAG_SYN) != 0u) && ((flags & NETD_TCP_FLAG_ACK) != 0u)) {
            if (ack != ctx->tcp.snd_nxt) {
                return;
            }

            ctx->tcp.irs = seq;
            ctx->tcp.rcv_nxt = seq + 1u;
            ctx->tcp.snd_una = ack;
            ctx->tcp.state = NETD_TCP_STATE_ESTABLISHED;
            netd_tcp_send_ack(ctx);
        }
        return;
    }

    if (ctx->tcp.state == NETD_TCP_STATE_ESTABLISHED || ctx->tcp.state == NETD_TCP_STATE_FIN_WAIT_1 || ctx->tcp.state == NETD_TCP_STATE_FIN_WAIT_2) {
        if (seg_data_len > 0) {
            if (seq != ctx->tcp.rcv_nxt) {
                netd_tcp_send_ack(ctx);
                return;
            }

            uint32_t space = netd_tcp_rx_space(&ctx->tcp);
            if (seg_data_len > space) {
                netd_tcp_send_ack(ctx);
                ctx->tcp.last_activity_ms = uptime_ms();
                return;
            }

            (void)netd_tcp_rx_write(&ctx->tcp, seg_data, seg_data_len);
            ctx->tcp.rcv_nxt += seg_data_len;
            netd_tcp_send_ack(ctx);
        }

        if ((flags & NETD_TCP_FLAG_FIN) != 0u) {
            if (seq == ctx->tcp.rcv_nxt) {
                ctx->tcp.rcv_nxt += 1u;
            } else if (seq + seg_data_len == ctx->tcp.rcv_nxt) {
                ctx->tcp.rcv_nxt += 1u;
            }

            ctx->tcp.remote_closed = 1;
            netd_tcp_send_ack(ctx);

            if (ctx->tcp.state == NETD_TCP_STATE_ESTABLISHED) {
                ctx->tcp.state = NETD_TCP_STATE_CLOSE_WAIT;
            }
            if (ctx->tcp.state == NETD_TCP_STATE_FIN_WAIT_1 && ctx->tcp.fin_acked) {
                ctx->tcp.state = NETD_TCP_STATE_FIN_WAIT_2;
            }
        }

        ctx->tcp.last_activity_ms = uptime_ms();
        return;
    }

    if (ctx->tcp.state == NETD_TCP_STATE_CLOSE_WAIT || ctx->tcp.state == NETD_TCP_STATE_LAST_ACK) {
        ctx->tcp.last_activity_ms = uptime_ms();
        return;
    }
}

int netd_tcp_connect(netd_ctx_t* ctx, uint32_t dst_ip, uint16_t dst_port, uint32_t timeout_ms) {
    if (!ctx) {
        return 0;
    }

    if (netd_iface_ensure_up(ctx) != 0) {
        ctx->tcp.last_err = NET_STATUS_UNREACHABLE;
        return 0;
    }

    netd_tcp_reset(ctx);
    ctx->tcp.last_err = NET_STATUS_ERROR;

    uint16_t local_port = (uint16_t)(49152u + (uint16_t)(uptime_ms() & 0x0FFFu));
    if (local_port == 0) {
        local_port = 49152u;
    }

    ctx->tcp.active = 1;
    ctx->tcp.state = NETD_TCP_STATE_SYN_SENT;
    ctx->tcp.remote_ip = dst_ip;
    ctx->tcp.remote_port = dst_port;
    ctx->tcp.local_port = local_port;

    uint32_t iss = (uint32_t)(uptime_ms() * 1103515245u + 12345u);
    if (iss == 0) {
        iss = 1u;
    }

    ctx->tcp.iss = iss;
    ctx->tcp.snd_una = iss;
    ctx->tcp.snd_nxt = iss;
    ctx->tcp.rcv_nxt = 0;

    if (!netd_tcp_send_segment(ctx, NETD_TCP_FLAG_SYN, 0, 0)) {
        netd_tcp_reset(ctx);
        ctx->tcp.last_err = NET_STATUS_ERROR;
        return 0;
    }

    ctx->tcp.snd_nxt += 1u;

    if (timeout_ms == 0) {
        timeout_ms = 3000u;
    }

    uint32_t start_ms = uptime_ms();
    for (;;) {
        netd_device_process(ctx);

        if (ctx->tcp.active && ctx->tcp.state == NETD_TCP_STATE_ESTABLISHED) {
            ctx->tcp.last_err = NET_STATUS_OK;
            return 1;
        }

        uint32_t now_ms = uptime_ms();
        if ((now_ms - start_ms) >= timeout_ms) {
            break;
        }

        uint32_t remaining_ms = timeout_ms - (now_ms - start_ms);
        uint32_t wait_ms_u32 = remaining_ms;
        if (wait_ms_u32 > 10u) {
            wait_ms_u32 = 10u;
        }
        int wait_ms = (int)wait_ms_u32;
        if (wait_ms <= 0) {
            break;
        }
        sleep(wait_ms);
    }

    netd_tcp_reset(ctx);
    ctx->tcp.last_err = NET_STATUS_TIMEOUT;
    return 0;
}

int netd_tcp_send(netd_ctx_t* ctx, const void* data, uint32_t len, uint32_t timeout_ms) {
    if (!ctx || !ctx->tcp.active || ctx->tcp.state != NETD_TCP_STATE_ESTABLISHED) {
        if (ctx) {
            ctx->tcp.last_err = NET_STATUS_ERROR;
        }
        return 0;
    }
    if (!data && len != 0) {
        ctx->tcp.last_err = NET_STATUS_ERROR;
        return 0;
    }

    const uint8_t* p = (const uint8_t*)data;

    if (timeout_ms == 0) {
        timeout_ms = 5000u;
    }

    uint32_t start_ms = uptime_ms();

    uint32_t off = 0;
    while (off < len) {
        uint32_t chunk = len - off;
        if (chunk > NETD_TCP_MSS) {
            chunk = NETD_TCP_MSS;
        }

        if (!netd_tcp_send_segment(ctx, (uint8_t)(NETD_TCP_FLAG_ACK | NETD_TCP_FLAG_PSH), p + off, chunk)) {
            ctx->tcp.last_err = NET_STATUS_ERROR;
            return 0;
        }

        ctx->tcp.snd_nxt += chunk;

        while (ctx->tcp.snd_una != ctx->tcp.snd_nxt) {
            uint32_t now_ms = uptime_ms();
            if ((now_ms - start_ms) >= timeout_ms) {
                ctx->tcp.last_err = NET_STATUS_TIMEOUT;
                return 0;
            }

            netd_device_process(ctx);

            uint32_t remaining_ms = timeout_ms - (now_ms - start_ms);
            uint32_t wait_ms_u32 = remaining_ms;
            if (wait_ms_u32 > 10u) {
                wait_ms_u32 = 10u;
            }
            int wait_ms = (int)wait_ms_u32;
            if (wait_ms <= 0) {
                ctx->tcp.last_err = NET_STATUS_TIMEOUT;
                return 0;
            }
            sleep(wait_ms);
        }

        off += chunk;
    }

    ctx->tcp.last_err = NET_STATUS_OK;
    return 1;
}

int netd_tcp_recv(netd_ctx_t* ctx, void* out, uint32_t cap, uint32_t timeout_ms, uint32_t* out_n) {
    if (out_n) {
        *out_n = 0;
    }

    if (!ctx || !ctx->tcp.active) {
        if (ctx) {
            ctx->tcp.last_err = NET_STATUS_ERROR;
        }
        return 0;
    }
    if (!out && cap != 0) {
        ctx->tcp.last_err = NET_STATUS_ERROR;
        return 0;
    }

    if (timeout_ms == 0) {
        timeout_ms = 5000u;
    }

    uint32_t start_ms = uptime_ms();
    for (;;) {
        uint32_t space_before = netd_tcp_rx_space(&ctx->tcp);

        uint32_t got = netd_tcp_rx_read(&ctx->tcp, (uint8_t*)out, cap);
        if (got > 0) {
            if (space_before == 0) {
                uint32_t space_after = netd_tcp_rx_space(&ctx->tcp);
                if (space_after > 0) {
                    netd_tcp_send_ack(ctx);
                }
            }
            if (out_n) {
                *out_n = got;
            }
            ctx->tcp.last_err = NET_STATUS_OK;
            return 1;
        }

        if (ctx->tcp.remote_closed) {
            if (out_n) {
                *out_n = 0;
            }
            ctx->tcp.last_err = NET_STATUS_OK;
            return 1;
        }

        netd_device_process(ctx);

        uint32_t now_ms = uptime_ms();
        if ((now_ms - start_ms) >= timeout_ms) {
            break;
        }

        uint32_t remaining_ms = timeout_ms - (now_ms - start_ms);
        uint32_t wait_ms_u32 = remaining_ms;
        if (wait_ms_u32 > 10u) {
            wait_ms_u32 = 10u;
        }
        int wait_ms = (int)wait_ms_u32;
        if (wait_ms <= 0) {
            break;
        }
        sleep(wait_ms);
    }

    ctx->tcp.last_err = NET_STATUS_TIMEOUT;
    return 0;
}

int netd_tcp_close(netd_ctx_t* ctx, uint32_t timeout_ms) {
    if (!ctx) {
        return 0;
    }

    if (!ctx->tcp.active) {
        ctx->tcp.last_err = NET_STATUS_OK;
        return 1;
    }

    if (ctx->tcp.state == NETD_TCP_STATE_ESTABLISHED || ctx->tcp.state == NETD_TCP_STATE_CLOSE_WAIT) {
        if (!ctx->tcp.fin_sent) {
            if (!netd_tcp_send_segment(ctx, (uint8_t)(NETD_TCP_FLAG_FIN | NETD_TCP_FLAG_ACK), 0, 0)) {
                netd_tcp_reset(ctx);
                ctx->tcp.last_err = NET_STATUS_ERROR;
                return 0;
            }
            ctx->tcp.fin_sent = 1;
            ctx->tcp.snd_nxt += 1u;
            ctx->tcp.state = NETD_TCP_STATE_FIN_WAIT_1;
        }
    }

    if (timeout_ms == 0) {
        timeout_ms = 3000u;
    }

    uint32_t start_ms = uptime_ms();
    for (;;) {
        netd_device_process(ctx);

        if (ctx->tcp.fin_sent && ctx->tcp.fin_acked && ctx->tcp.remote_closed) {
            netd_tcp_reset(ctx);
            ctx->tcp.last_err = NET_STATUS_OK;
            return 1;
        }

        if (ctx->tcp.fin_sent && ctx->tcp.fin_acked && !ctx->tcp.remote_closed) {
            ctx->tcp.state = NETD_TCP_STATE_FIN_WAIT_2;
        }

        uint32_t now_ms = uptime_ms();
        if ((now_ms - start_ms) >= timeout_ms) {
            break;
        }

        uint32_t remaining_ms = timeout_ms - (now_ms - start_ms);
        uint32_t wait_ms_u32 = remaining_ms;
        if (wait_ms_u32 > 10u) {
            wait_ms_u32 = 10u;
        }
        int wait_ms = (int)wait_ms_u32;
        if (wait_ms <= 0) {
            break;
        }
        sleep(wait_ms);
    }

    netd_tcp_reset(ctx);
    ctx->tcp.last_err = NET_STATUS_TIMEOUT;
    return 0;
}
