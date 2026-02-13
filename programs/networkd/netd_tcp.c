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

#define NETD_TCP_MAP_EMPTY 0u
#define NETD_TCP_MAP_TOMBSTONE 0xFFFFFFFFu

static uint32_t netd_tcp_hash(uint32_t src_ip, uint16_t src_port, uint16_t dst_port) {
    uint32_t x = src_ip;
    x ^= ((uint32_t)src_port << 16) | (uint32_t)dst_port;

    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

static uint32_t netd_tcp_round_pow2(uint32_t v) {
    if (v < 2u) {
        return 2u;
    }

    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v++;
    return v;
}

static void netd_tcp_mgr_init(netd_tcp_mgr_t* m) {
    if (!m) {
        return;
    }

    m->conns = 0;
    m->count = 0;
    m->cap = 0;
    m->map = 0;
    m->map_cap = 0;
}

static void netd_tcp_mgr_free(netd_tcp_mgr_t* m) {
    if (!m) {
        return;
    }

    if (m->conns) {
        for (uint32_t i = 0; i < m->count; i++) {
            free(m->conns[i]);
        }
        free(m->conns);
    }

    if (m->map) {
        free(m->map);
    }

    netd_tcp_mgr_init(m);
}

static int netd_tcp_mgr_reserve_conns(netd_tcp_mgr_t* m, uint32_t need) {
    if (!m) {
        return 0;
    }

    if (need <= m->cap) {
        return 1;
    }

    uint32_t new_cap = (m->cap == 0) ? 4u : m->cap;
    while (new_cap < need) {
        if (new_cap > 0x80000000u) {
            return 0;
        }
        new_cap *= 2u;
    }

    size_t bytes = (size_t)new_cap * sizeof(m->conns[0]);
    netd_tcp_conn_t** p = (netd_tcp_conn_t**)realloc(m->conns, bytes);
    if (!p) {
        return 0;
    }

    if (new_cap > m->cap) {
        memset(p + m->cap, 0, (size_t)(new_cap - m->cap) * sizeof(p[0]));
    }

    m->conns = p;
    m->cap = new_cap;
    return 1;
}

static int netd_tcp_mgr_reserve_map(netd_tcp_mgr_t* m, uint32_t need_live) {
    if (!m) {
        return 0;
    }

    uint32_t target = netd_tcp_round_pow2(need_live * 2u);
    if (target <= m->map_cap) {
        return 1;
    }

    uint32_t* new_map = (uint32_t*)malloc((size_t)target * sizeof(uint32_t));
    if (!new_map) {
        return 0;
    }
    for (uint32_t i = 0; i < target; i++) {
        new_map[i] = NETD_TCP_MAP_EMPTY;
    }

    if (m->map && m->map_cap != 0) {
        for (uint32_t i = 0; i < m->map_cap; i++) {
            uint32_t tag = m->map[i];
            if (tag == NETD_TCP_MAP_EMPTY || tag == NETD_TCP_MAP_TOMBSTONE) {
                continue;
            }

            uint32_t idx = tag - 1u;
            if (idx >= m->count) {
                continue;
            }

            netd_tcp_conn_t* c = m->conns[idx];
            if (!c || !c->active) {
                continue;
            }

            uint32_t h = netd_tcp_hash(c->remote_ip, c->remote_port, c->local_port);
            uint32_t mask = target - 1u;
            for (uint32_t step = 0; step < target; step++) {
                uint32_t pos = (h + step) & mask;
                if (new_map[pos] == NETD_TCP_MAP_EMPTY) {
                    new_map[pos] = tag;
                    break;
                }
            }
        }
    }

    free(m->map);
    m->map = new_map;
    m->map_cap = target;
    return 1;
}

static int netd_tcp_mgr_map_insert(netd_tcp_mgr_t* m, netd_tcp_conn_t* c) {
    if (!m || !c) {
        return 0;
    }

    if (!netd_tcp_mgr_reserve_map(m, m->count)) {
        return 0;
    }

    uint32_t h = netd_tcp_hash(c->remote_ip, c->remote_port, c->local_port);
    uint32_t mask = m->map_cap - 1u;

    uint32_t first_tomb = NETD_TCP_MAP_EMPTY;
    for (uint32_t step = 0; step < m->map_cap; step++) {
        uint32_t pos = (h + step) & mask;
        uint32_t v = m->map[pos];

        if (v == NETD_TCP_MAP_EMPTY) {
            if (first_tomb != NETD_TCP_MAP_EMPTY) {
                pos = first_tomb;
            }
            m->map[pos] = c->mgr_index + 1u;
            return 1;
        }

        if (v == NETD_TCP_MAP_TOMBSTONE) {
            if (first_tomb == NETD_TCP_MAP_EMPTY) {
                first_tomb = pos;
            }
            continue;
        }

        if (v == c->mgr_index + 1u) {
            return 1;
        }
    }

    if (first_tomb != NETD_TCP_MAP_EMPTY) {
        m->map[first_tomb] = c->mgr_index + 1u;
        return 1;
    }

    return 0;
}

static void netd_tcp_mgr_map_erase(netd_tcp_mgr_t* m, const netd_tcp_conn_t* c) {
    if (!m || !m->map || m->map_cap == 0 || !c) {
        return;
    }

    uint32_t tag = c->mgr_index + 1u;
    uint32_t h = netd_tcp_hash(c->remote_ip, c->remote_port, c->local_port);
    uint32_t mask = m->map_cap - 1u;

    for (uint32_t step = 0; step < m->map_cap; step++) {
        uint32_t pos = (h + step) & mask;
        uint32_t v = m->map[pos];

        if (v == NETD_TCP_MAP_EMPTY) {
            return;
        }

        if (v == tag) {
            m->map[pos] = NETD_TCP_MAP_TOMBSTONE;
            return;
        }
    }
}

static netd_tcp_conn_t* netd_tcp_mgr_lookup(const netd_tcp_mgr_t* m, uint32_t src_ip, uint16_t src_port, uint16_t dst_port) {
    if (!m || !m->map || m->map_cap == 0) {
        return 0;
    }

    uint32_t h = netd_tcp_hash(src_ip, src_port, dst_port);
    uint32_t mask = m->map_cap - 1u;

    for (uint32_t step = 0; step < m->map_cap; step++) {
        uint32_t pos = (h + step) & mask;
        uint32_t v = m->map[pos];

        if (v == NETD_TCP_MAP_EMPTY) {
            return 0;
        }
        if (v == NETD_TCP_MAP_TOMBSTONE) {
            continue;
        }

        uint32_t idx = v - 1u;
        if (idx >= m->count) {
            continue;
        }

        netd_tcp_conn_t* c = m->conns[idx];
        if (!c || !c->active) {
            continue;
        }

        if (c->remote_ip == src_ip && c->remote_port == src_port && c->local_port == dst_port) {
            return c;
        }
    }

    return 0;
}

static uint32_t netd_tcp_rx_count(const netd_tcp_conn_t* c) {
    if (!c) {
        return 0;
    }

    if (c->rx_w >= c->rx_r) {
        return c->rx_w - c->rx_r;
    }

    return c->rx_cap - (c->rx_r - c->rx_w);
}

static uint32_t netd_tcp_rx_space(const netd_tcp_conn_t* c) {
    uint32_t used = netd_tcp_rx_count(c);
    if (used >= c->rx_cap - 1u) {
        return 0;
    }
    return c->rx_cap - 1u - used;
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
    uint32_t first = c->rx_cap - wi;
    if (first > len) {
        first = len;
    }

    memcpy(c->rx_buf + wi, data, first);

    if (len > first) {
        memcpy(c->rx_buf, data + first, len - first);
    }

    wi += len;
    if (wi >= c->rx_cap) {
        wi -= c->rx_cap;
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
    uint32_t first = c->rx_cap - ri;
    if (first > cap) {
        first = cap;
    }

    memcpy(out, c->rx_buf + ri, first);

    if (cap > first) {
        memcpy(out + first, c->rx_buf, cap - first);
    }

    ri += cap;
    if (ri >= c->rx_cap) {
        ri -= c->rx_cap;
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

static int netd_tcp_mgr_alloc_slot(netd_ctx_t* ctx, uint32_t* out_index);

static void netd_tcp_conn_reset(netd_tcp_conn_t* c) {
    if (!c) {
        return;
    }

    uint32_t idx = c->mgr_index;
    memset(c, 0, sizeof(*c));
    c->mgr_index = idx;
}

static int netd_tcp_send_segment(netd_ctx_t* ctx, netd_tcp_conn_t* c, uint8_t flags, const uint8_t* payload, uint32_t payload_len) {
    if (!ctx || !c || !ctx->iface.up) {
        return 0;
    }
    if (!c->active) {
        return 0;
    }
    if (payload_len > 0 && !payload) {
        return 0;
    }

    uint32_t ip_payload_len = (uint32_t)sizeof(net_tcp_hdr_t) + payload_len;
    uint32_t ip_total_len = (uint32_t)sizeof(net_ipv4_hdr_t) + ip_payload_len;
    uint32_t frame_len = (uint32_t)sizeof(net_eth_hdr_t) + ip_total_len;

    if (frame_len > ctx->tx_buf_size) {
        return 0;
    }

    uint32_t next_hop = netd_iface_next_hop_ip(ctx, c->remote_ip);
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
    ip->dst = netd_htonl(c->remote_ip);
    ip->hdr_checksum = 0;
    ip->hdr_checksum = netd_htons(netd_checksum16(ip, sizeof(net_ipv4_hdr_t)));

    tcp->src_port = netd_htons(c->local_port);
    tcp->dst_port = netd_htons(c->remote_port);
    tcp->seq = netd_htonl(c->snd_nxt);
    tcp->ack = netd_htonl(c->rcv_nxt);
    tcp->data_offset = (uint8_t)(5u << 4);
    tcp->flags = flags;
    tcp->window = netd_htons(netd_tcp_window(c));
    tcp->checksum = 0;
    tcp->urg_ptr = 0;

    if (payload_len > 0) {
        memcpy(out, payload, payload_len);
    }

    uint16_t csum = netd_tcp_checksum(ip, (const uint8_t*)tcp, (uint32_t)sizeof(net_tcp_hdr_t), payload, payload_len);
    tcp->checksum = netd_htons(csum);

    int sent = netd_iface_send_frame(ctx, ctx->tx_buf, frame_len) > 0;
    if (sent) {
        c->last_activity_ms = uptime_ms();
    }
    return sent;
}

static void netd_tcp_send_ack(netd_ctx_t* ctx, netd_tcp_conn_t* c) {
    if (!ctx || !c || !c->active) {
        return;
    }
    (void)netd_tcp_send_segment(ctx, c, NETD_TCP_FLAG_ACK, 0, 0);
}

static void netd_tcp_conn_destroy(netd_ctx_t* ctx, netd_tcp_conn_t* c) {
    if (!ctx || !c) {
        return;
    }

    uint32_t idx = c->mgr_index;
    if (idx < ctx->tcp.count && ctx->tcp.conns[idx] == c) {
        ctx->tcp.conns[idx] = 0;
    }

    if (c->rx_buf) {
        free(c->rx_buf);
    }

    if (c->tx_buf) {
        free(c->tx_buf);
    }

    free(c);
}

static netd_tcp_conn_t* netd_tcp_open_create_and_send_syn(netd_ctx_t* ctx, uint32_t dst_ip, uint16_t dst_port, uint32_t* out_status) {
    if (out_status) {
        *out_status = NET_STATUS_ERROR;
    }

    if (!ctx) {
        return 0;
    }

    if (netd_iface_ensure_up(ctx) != 0) {
        if (out_status) {
            *out_status = NET_STATUS_UNREACHABLE;
        }
        return 0;
    }

    netd_tcp_conn_t* c = (netd_tcp_conn_t*)calloc(1, sizeof(*c));
    if (!c) {
        if (out_status) {
            *out_status = NET_STATUS_ERROR;
        }
        return 0;
    }

    c->rx_cap = NETD_TCP_RX_BUF_DEFAULT;
    c->rx_buf = (uint8_t*)malloc(c->rx_cap);
    if (!c->rx_buf) {
        free(c);
        if (out_status) {
            *out_status = NET_STATUS_ERROR;
        }
        return 0;
    }

    c->tx_cap = NETD_TCP_TX_BUF_DEFAULT;
    c->tx_buf = (uint8_t*)malloc(c->tx_cap);
    if (!c->tx_buf) {
        free(c->rx_buf);
        free(c);
        if (out_status) {
            *out_status = NET_STATUS_ERROR;
        }
        return 0;
    }

    uint32_t slot = 0;
    if (!netd_tcp_mgr_alloc_slot(ctx, &slot)) {
        free(c);
        if (out_status) {
            *out_status = NET_STATUS_ERROR;
        }
        return 0;
    }

    c->mgr_index = slot;
    ctx->tcp.conns[slot] = c;

    uint16_t local_port = (uint16_t)(49152u + (uint16_t)(uptime_ms() & 0x0FFFu));
    if (local_port == 0) {
        local_port = 49152u;
    }

    c->active = 1;
    c->state = NETD_TCP_STATE_SYN_SENT;
    c->remote_ip = dst_ip;
    c->remote_port = dst_port;
    c->local_port = local_port;
    c->last_err = NET_STATUS_ERROR;

    uint32_t iss = (uint32_t)(uptime_ms() * 1103515245u + 12345u);
    if (iss == 0) {
        iss = 1u;
    }

    c->iss = iss;
    c->snd_una = iss;
    c->snd_nxt = iss;
    c->rcv_nxt = 0;

    if (!netd_tcp_mgr_map_insert(&ctx->tcp, c)) {
        netd_tcp_conn_destroy(ctx, c);
        if (out_status) {
            *out_status = NET_STATUS_ERROR;
        }
        return 0;
    }

    if (!netd_tcp_send_segment(ctx, c, NETD_TCP_FLAG_SYN, 0, 0)) {
        netd_tcp_mgr_map_erase(&ctx->tcp, c);
        netd_tcp_conn_destroy(ctx, c);
        if (out_status) {
            *out_status = NET_STATUS_ERROR;
        }
        return 0;
    }

    c->snd_nxt += 1u;

    if (out_status) {
        *out_status = NET_STATUS_OK;
    }
    return c;
}

netd_tcp_conn_t* netd_tcp_open_start(netd_ctx_t* ctx, uint32_t dst_ip, uint16_t dst_port, uint32_t* out_status) {
    return netd_tcp_open_create_and_send_syn(ctx, dst_ip, dst_port, out_status);
}

int netd_tcp_open_poll(netd_ctx_t* ctx, netd_tcp_conn_t* c, uint32_t start_ms, uint32_t timeout_ms, uint32_t* out_status) {
    if (out_status) {
        *out_status = NET_STATUS_ERROR;
    }
    if (!ctx || !c) {
        return 1;
    }

    if (!c->active) {
        if (out_status) {
            *out_status = (c->last_err != 0) ? c->last_err : NET_STATUS_ERROR;
        }
        return 1;
    }

    if (c->state == NETD_TCP_STATE_ESTABLISHED) {
        c->last_err = NET_STATUS_OK;
        if (out_status) {
            *out_status = NET_STATUS_OK;
        }
        return 1;
    }

    if (timeout_ms == 0) {
        timeout_ms = 3000u;
    }

    uint32_t now_ms = uptime_ms();
    if ((now_ms - start_ms) >= timeout_ms) {
        c->last_err = NET_STATUS_TIMEOUT;
        if (out_status) {
            *out_status = NET_STATUS_TIMEOUT;
        }
        netd_tcp_mgr_map_erase(&ctx->tcp, c);
        netd_tcp_conn_destroy(ctx, c);
        return 1;
    }

    if (out_status) {
        *out_status = NET_STATUS_OK;
    }
    return 0;
}

uint32_t netd_tcp_recv_nowait(netd_tcp_conn_t* c, void* out, uint32_t cap) {
    if (!c || !out || cap == 0) {
        return 0;
    }
    return netd_tcp_rx_read(c, (uint8_t*)out, cap);
}

int netd_tcp_send_poll(
    netd_ctx_t* ctx,
    netd_tcp_conn_t* c,
    const void* data,
    uint32_t len,
    uint32_t* io_off,
    uint32_t start_ms,
    uint32_t timeout_ms,
    uint32_t* out_status
) {
    if (out_status) {
        *out_status = NET_STATUS_ERROR;
    }
    if (!io_off) {
        if (out_status) {
            *out_status = NET_STATUS_ERROR;
        }
        return 1;
    }

    if (!ctx || !c || !c->active || c->state != NETD_TCP_STATE_ESTABLISHED) {
        if (out_status) {
            *out_status = (c && c->last_err != 0) ? c->last_err : NET_STATUS_ERROR;
        }
        return 1;
    }
    if (!data && len != 0) {
        c->last_err = NET_STATUS_ERROR;
        if (out_status) {
            *out_status = NET_STATUS_ERROR;
        }
        return 1;
    }

    if (*io_off >= len) {
        c->last_err = NET_STATUS_OK;
        if (out_status) {
            *out_status = NET_STATUS_OK;
        }
        return 1;
    }

    if (timeout_ms == 0) {
        timeout_ms = 5000u;
    }

    uint32_t now_ms = uptime_ms();
    if ((now_ms - start_ms) >= timeout_ms) {
        c->last_err = NET_STATUS_TIMEOUT;
        if (out_status) {
            *out_status = NET_STATUS_TIMEOUT;
        }
        return 1;
    }

    if (c->snd_una != c->snd_nxt) {
        if (out_status) {
            *out_status = NET_STATUS_OK;
        }
        return 0;
    }

    uint32_t remaining = len - *io_off;
    uint32_t chunk = remaining;
    if (chunk > NETD_TCP_MSS) {
        chunk = NETD_TCP_MSS;
    }

    const uint8_t* p = (const uint8_t*)data;
    if (!netd_tcp_send_segment(ctx, c, (uint8_t)(NETD_TCP_FLAG_ACK | NETD_TCP_FLAG_PSH), p + *io_off, chunk)) {
        c->last_err = NET_STATUS_ERROR;
        if (out_status) {
            *out_status = NET_STATUS_ERROR;
        }
        return 1;
    }

    c->snd_nxt += chunk;
    *io_off += chunk;

    c->last_err = NET_STATUS_OK;
    if (out_status) {
        *out_status = NET_STATUS_OK;
    }
    return (*io_off >= len) ? 1 : 0;
}

int netd_tcp_close_start(netd_ctx_t* ctx, netd_tcp_conn_t* c, uint32_t* out_status) {
    if (out_status) {
        *out_status = NET_STATUS_ERROR;
    }
    if (!ctx || !c) {
        return 1;
    }
    if (!c->active) {
        c->last_err = NET_STATUS_OK;
        if (out_status) {
            *out_status = NET_STATUS_OK;
        }
        return 1;
    }

    if (c->state == NETD_TCP_STATE_ESTABLISHED || c->state == NETD_TCP_STATE_CLOSE_WAIT) {
        if (!c->fin_sent) {
            if (!netd_tcp_send_segment(ctx, c, (uint8_t)(NETD_TCP_FLAG_FIN | NETD_TCP_FLAG_ACK), 0, 0)) {
                netd_tcp_mgr_map_erase(&ctx->tcp, c);
                c->last_err = NET_STATUS_ERROR;
                netd_tcp_conn_destroy(ctx, c);
                if (out_status) {
                    *out_status = NET_STATUS_ERROR;
                }
                return 1;
            }
            c->fin_sent = 1;
            c->snd_nxt += 1u;
            c->state = NETD_TCP_STATE_FIN_WAIT_1;
        }
    }

    c->last_err = NET_STATUS_OK;
    if (out_status) {
        *out_status = NET_STATUS_OK;
    }
    return 0;
}

int netd_tcp_close_poll(netd_ctx_t* ctx, netd_tcp_conn_t* c, uint32_t start_ms, uint32_t timeout_ms, uint32_t* out_status) {
    if (out_status) {
        *out_status = NET_STATUS_ERROR;
    }
    if (!ctx || !c) {
        return 1;
    }

    if (!c->active) {
        c->last_err = NET_STATUS_OK;
        if (out_status) {
            *out_status = NET_STATUS_OK;
        }
        return 1;
    }

    if (c->fin_sent && c->fin_acked && c->remote_closed) {
        netd_tcp_mgr_map_erase(&ctx->tcp, c);
        c->last_err = NET_STATUS_OK;
        netd_tcp_conn_destroy(ctx, c);
        if (out_status) {
            *out_status = NET_STATUS_OK;
        }
        return 1;
    }

    if (c->fin_sent && c->fin_acked && !c->remote_closed) {
        c->state = NETD_TCP_STATE_FIN_WAIT_2;
    }

    if (timeout_ms == 0) {
        timeout_ms = 3000u;
    }

    uint32_t now_ms = uptime_ms();
    if ((now_ms - start_ms) >= timeout_ms) {
        netd_tcp_mgr_map_erase(&ctx->tcp, c);
        c->last_err = NET_STATUS_TIMEOUT;
        netd_tcp_conn_destroy(ctx, c);
        if (out_status) {
            *out_status = NET_STATUS_TIMEOUT;
        }
        return 1;
    }

    c->last_err = NET_STATUS_OK;
    if (out_status) {
        *out_status = NET_STATUS_OK;
    }
    return 0;
}

static int netd_tcp_mgr_alloc_slot(netd_ctx_t* ctx, uint32_t* out_index) {
    if (out_index) {
        *out_index = 0;
    }

    if (!ctx || !out_index) {
        return 0;
    }

    for (uint32_t i = 0; i < ctx->tcp.count; i++) {
        if (ctx->tcp.conns[i] == 0) {
            *out_index = i;
            return 1;
        }
    }

    if (!netd_tcp_mgr_reserve_conns(&ctx->tcp, ctx->tcp.count + 1u)) {
        return 0;
    }

    *out_index = ctx->tcp.count;
    ctx->tcp.count++;
    return 1;
}

void netd_tcp_init(netd_ctx_t* ctx) {
    if (!ctx) {
        return;
    }
    netd_tcp_mgr_init(&ctx->tcp);
}

void netd_tcp_shutdown(netd_ctx_t* ctx) {
    if (!ctx) {
        return;
    }
    netd_tcp_mgr_free(&ctx->tcp);
}

void netd_tcp_cleanup_idle(netd_ctx_t* ctx) {
    if (!ctx) {
        return;
    }

    netd_tcp_mgr_t* mgr = &ctx->tcp;
    if (!mgr->conns) {
        return;
    }

    uint32_t now = uptime_ms();
    uint32_t idle_timeout = NETD_TCP_IDLE_TIMEOUT_MS;
    uint32_t conn_timeout = NETD_TCP_CONN_TIMEOUT_MS;

    for (uint32_t i = 0; i < mgr->count; i++) {
        netd_tcp_conn_t* c = mgr->conns[i];
        if (!c || !c->active) {
            continue;
        }

        uint32_t idle_time = now - c->last_activity_ms;
        
        if (c->state == NETD_TCP_STATE_ESTABLISHED) {
            if (idle_time > idle_timeout) {
                netd_tcp_conn_destroy(ctx, c);
                continue;
            }
        } else {
            if (idle_time > conn_timeout) {
                netd_tcp_conn_destroy(ctx, c);
                continue;
            }
        }
    }
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

    netd_tcp_conn_t* c = netd_tcp_mgr_lookup(&ctx->tcp, src_ip, src_port, dst_port);
    if (!c) {
        return;
    }

    uint8_t flags = tcp->flags;
    uint32_t seq = netd_ntohl(tcp->seq);
    uint32_t ack = netd_ntohl(tcp->ack);

    if ((flags & NETD_TCP_FLAG_RST) != 0u) {
        netd_tcp_mgr_map_erase(&ctx->tcp, c);
        netd_tcp_conn_destroy(ctx, c);
        return;
    }

    if (ack > c->snd_una && ack <= c->snd_nxt) {
        c->snd_una = ack;
        if (c->fin_sent && c->snd_una == c->snd_nxt) {
            c->fin_acked = 1;
        }
    }

    if (c->state == NETD_TCP_STATE_SYN_SENT) {
        if (((flags & NETD_TCP_FLAG_SYN) != 0u) && ((flags & NETD_TCP_FLAG_ACK) != 0u)) {
            if (ack != c->snd_nxt) {
                return;
            }

            c->irs = seq;
            c->rcv_nxt = seq + 1u;
            c->snd_una = ack;
            c->state = NETD_TCP_STATE_ESTABLISHED;
            netd_tcp_send_ack(ctx, c);
        }
        return;
    }

    if (c->state == NETD_TCP_STATE_ESTABLISHED || c->state == NETD_TCP_STATE_FIN_WAIT_1 || c->state == NETD_TCP_STATE_FIN_WAIT_2) {
        if (seg_data_len > 0) {
            if (seq != c->rcv_nxt) {
                netd_tcp_send_ack(ctx, c);
                return;
            }

            uint32_t space = netd_tcp_rx_space(c);
            if (seg_data_len > space) {
                netd_tcp_send_ack(ctx, c);
                c->last_activity_ms = uptime_ms();
                return;
            }

            (void)netd_tcp_rx_write(c, seg_data, seg_data_len);
            c->rcv_nxt += seg_data_len;
            netd_tcp_send_ack(ctx, c);
        }

        if ((flags & NETD_TCP_FLAG_FIN) != 0u) {
            if (seq == c->rcv_nxt) {
                c->rcv_nxt += 1u;
            } else if (seq + seg_data_len == c->rcv_nxt) {
                c->rcv_nxt += 1u;
            }

            c->remote_closed = 1;
            netd_tcp_send_ack(ctx, c);

            if (c->state == NETD_TCP_STATE_ESTABLISHED) {
                c->state = NETD_TCP_STATE_CLOSE_WAIT;
            }
            if (c->state == NETD_TCP_STATE_FIN_WAIT_1 && c->fin_acked) {
                c->state = NETD_TCP_STATE_FIN_WAIT_2;
            }
        }

        c->last_activity_ms = uptime_ms();
        return;
    }

    if (c->state == NETD_TCP_STATE_CLOSE_WAIT || c->state == NETD_TCP_STATE_LAST_ACK) {
        c->last_activity_ms = uptime_ms();
        return;
    }
}

netd_tcp_conn_t* netd_tcp_open(netd_ctx_t* ctx, uint32_t dst_ip, uint16_t dst_port, uint32_t timeout_ms, uint32_t* out_status) {
    if (out_status) {
        *out_status = NET_STATUS_ERROR;
    }

    if (!ctx) {
        return 0;
    }

    netd_tcp_conn_t* c = netd_tcp_open_create_and_send_syn(ctx, dst_ip, dst_port, out_status);
    if (!c) {
        return 0;
    }

    if (timeout_ms == 0) {
        timeout_ms = 3000u;
    }

    uint32_t start_ms = uptime_ms();
    for (;;) {
        netd_device_process(ctx);

        if (c->active && c->state == NETD_TCP_STATE_ESTABLISHED) {
            c->last_err = NET_STATUS_OK;
            if (out_status) {
                *out_status = NET_STATUS_OK;
            }
            return c;
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

    c->last_err = NET_STATUS_TIMEOUT;
    if (out_status) {
        *out_status = NET_STATUS_TIMEOUT;
    }
    netd_tcp_mgr_map_erase(&ctx->tcp, c);
    netd_tcp_conn_destroy(ctx, c);
    return 0;
}

int netd_tcp_send(netd_ctx_t* ctx, netd_tcp_conn_t* c, const void* data, uint32_t len, uint32_t timeout_ms) {
    if (!ctx || !c || !c->active || c->state != NETD_TCP_STATE_ESTABLISHED) {
        if (c) {
            c->last_err = NET_STATUS_ERROR;
        }
        return 0;
    }
    if (!data && len != 0) {
        c->last_err = NET_STATUS_ERROR;
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

        if (!netd_tcp_send_segment(ctx, c, (uint8_t)(NETD_TCP_FLAG_ACK | NETD_TCP_FLAG_PSH), p + off, chunk)) {
            c->last_err = NET_STATUS_ERROR;
            return 0;
        }

        c->snd_nxt += chunk;

        while (c->snd_una != c->snd_nxt) {
            uint32_t now_ms = uptime_ms();
            if ((now_ms - start_ms) >= timeout_ms) {
                c->last_err = NET_STATUS_TIMEOUT;
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
                c->last_err = NET_STATUS_TIMEOUT;
                return 0;
            }
            sleep(wait_ms);
        }

        off += chunk;
    }

    c->last_err = NET_STATUS_OK;
    return 1;
}

int netd_tcp_recv(netd_ctx_t* ctx, netd_tcp_conn_t* c, void* out, uint32_t cap, uint32_t timeout_ms, uint32_t* out_n) {
    if (out_n) {
        *out_n = 0;
    }

    if (!ctx || !c || !c->active) {
        if (c) {
            c->last_err = NET_STATUS_ERROR;
        }
        return 0;
    }
    if (!out && cap != 0) {
        c->last_err = NET_STATUS_ERROR;
        return 0;
    }

    if (timeout_ms == 0) {
        timeout_ms = 5000u;
    }

    uint32_t start_ms = uptime_ms();
    for (;;) {
        uint32_t space_before = netd_tcp_rx_space(c);

        uint32_t got = netd_tcp_rx_read(c, (uint8_t*)out, cap);
        if (got > 0) {
            if (space_before == 0) {
                uint32_t space_after = netd_tcp_rx_space(c);
                if (space_after > 0) {
                    netd_tcp_send_ack(ctx, c);
                }
            }
            if (out_n) {
                *out_n = got;
            }
            c->last_err = NET_STATUS_OK;
            return 1;
        }

        if (c->remote_closed) {
            if (out_n) {
                *out_n = 0;
            }
            c->last_err = NET_STATUS_OK;
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

    c->last_err = NET_STATUS_TIMEOUT;
    return 0;
}

int netd_tcp_close(netd_ctx_t* ctx, netd_tcp_conn_t* c, uint32_t timeout_ms) {
    if (!ctx || !c) {
        return 0;
    }

    if (!c->active) {
        c->last_err = NET_STATUS_OK;
        return 1;
    }

    if (c->state == NETD_TCP_STATE_ESTABLISHED || c->state == NETD_TCP_STATE_CLOSE_WAIT) {
        if (!c->fin_sent) {
            if (!netd_tcp_send_segment(ctx, c, (uint8_t)(NETD_TCP_FLAG_FIN | NETD_TCP_FLAG_ACK), 0, 0)) {
                netd_tcp_mgr_map_erase(&ctx->tcp, c);
                c->last_err = NET_STATUS_ERROR;
                netd_tcp_conn_destroy(ctx, c);
                return 0;
            }
            c->fin_sent = 1;
            c->snd_nxt += 1u;
            c->state = NETD_TCP_STATE_FIN_WAIT_1;
        }
    }

    if (timeout_ms == 0) {
        timeout_ms = 3000u;
    }

    uint32_t start_ms = uptime_ms();
    for (;;) {
        netd_device_process(ctx);

        if (c->fin_sent && c->fin_acked && c->remote_closed) {
            netd_tcp_mgr_map_erase(&ctx->tcp, c);
            c->last_err = NET_STATUS_OK;
            netd_tcp_conn_destroy(ctx, c);
            return 1;
        }

        if (c->fin_sent && c->fin_acked && !c->remote_closed) {
            c->state = NETD_TCP_STATE_FIN_WAIT_2;
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

    netd_tcp_mgr_map_erase(&ctx->tcp, c);
    c->last_err = NET_STATUS_TIMEOUT;
    netd_tcp_conn_destroy(ctx, c);
    return 0;
}
