// SPDX-License-Identifier: GPL-2.0

#include "netd_dns.h"

#include <yula.h>

#include "netd_config.h"
#include "netd_device.h"
#include "netd_proto.h"
#include "netd_rand.h"
#include "netd_udp.h"
#include "netd_util.h"

static uint16_t netd_dns_gen_id(netd_ctx_t* ctx) {
    if (!ctx) {
        return (uint16_t)(uptime_ms() & 0xFFFFu);
    }

    uint16_t r = 0;
    netd_rand_bytes(&ctx->rand, &r, (uint32_t)sizeof(r));
    if (r == 0) {
        r = (uint16_t)(uptime_ms() & 0xFFFFu);
    }
    return r;
}

static uint16_t netd_dns_port_from_id(uint16_t id) {
    return (uint16_t)(49152u + (uint16_t)(id & 0x03FFu));
}

static netd_dns_wait_slot_t* netd_dns_slot_by_handle(netd_ctx_t* ctx, int handle) {
    if (!ctx || handle < 0 || handle >= NETD_DNS_MAX_WAITS) {
        return 0;
    }
    return &ctx->dns_waits[handle];
}

static int netd_dns_alloc_slot(netd_ctx_t* ctx) {
    if (!ctx) {
        return -1;
    }
    for (int i = 0; i < NETD_DNS_MAX_WAITS; i++) {
        if (!ctx->dns_waits[i].active) {
            return i;
        }
    }
    return -1;
}

static void netd_dns_slot_reset(netd_dns_wait_slot_t* s) {
    if (!s) {
        return;
    }
    memset(s, 0, sizeof(*s));
}

static uint32_t netd_dns_encode_qname(const char* name, uint8_t* out, uint32_t cap) {
    if (!name || !*name || !out || cap == 0) {
        return 0;
    }

    uint32_t w = 0;
    const char* p = name;

    for (;;) {
        const char* label_start = p;
        uint32_t label_len = 0;

        while (*p != '\0' && *p != '.') {
            label_len++;
            if (label_len > 63u) {
                return 0;
            }
            p++;
        }

        if (label_len == 0) {
            if (*p == '.') {
                p++;
                continue;
            }
            break;
        }

        if (w + 1u + label_len > cap) {
            return 0;
        }

        out[w] = (uint8_t)label_len;
        w++;
        memcpy(out + w, label_start, label_len);
        w += label_len;

        if (*p == '.') {
            p++;
            continue;
        }
        break;
    }

    if (w + 1u > cap) {
        return 0;
    }

    out[w] = 0;
    w++;
    return w;
}

static uint32_t netd_dns_build_query(uint16_t id, const char* name, uint8_t* out, uint32_t cap) {
    if (!name || !out) {
        return 0;
    }

    if (cap < sizeof(net_dns_hdr_t) + 1u + 4u) {
        return 0;
    }

    net_dns_hdr_t* hdr = (net_dns_hdr_t*)out;
    hdr->id = netd_htons(id);
    hdr->flags = netd_htons(0x0100u);
    hdr->qdcount = netd_htons(1u);
    hdr->ancount = 0;
    hdr->nscount = 0;
    hdr->arcount = 0;

    uint32_t off = (uint32_t)sizeof(net_dns_hdr_t);
    uint32_t qname_len = netd_dns_encode_qname(name, out + off, cap - off);
    if (qname_len == 0) {
        return 0;
    }
    off += qname_len;

    if (off + 4u > cap) {
        return 0;
    }

    uint16_t qtype = netd_htons(1u);
    uint16_t qclass = netd_htons(1u);
    memcpy(out + off, &qtype, 2u);
    memcpy(out + off + 2u, &qclass, 2u);
    off += 4u;

    return off;
}

static int netd_dns_skip_name(const uint8_t* msg, uint32_t msg_len, uint32_t* io_off) {
    if (!msg || !io_off) {
        return 0;
    }

    uint32_t off = *io_off;
    uint32_t jumps = 0;
    int jumped = 0;

    for (;;) {
        if (off >= msg_len) {
            return 0;
        }

        uint8_t b = msg[off];
        if (b == 0) {
            off++;
            if (!jumped) {
                *io_off = off;
            }
            return 1;
        }

        if ((b & 0xC0u) == 0xC0u) {
            if (off + 1u >= msg_len) {
                return 0;
            }

            uint16_t ptr = (uint16_t)(((uint16_t)(b & 0x3Fu) << 8) | (uint16_t)msg[off + 1u]);
            if (ptr >= msg_len) {
                return 0;
            }

            if (!jumped) {
                *io_off = off + 2u;
                jumped = 1;
            }

            off = (uint32_t)ptr;
            jumps++;
            if (jumps > 16u) {
                return 0;
            }
            continue;
        }

        if ((b & 0xC0u) != 0u) {
            return 0;
        }

        uint32_t label_len = (uint32_t)b;
        off++;
        if (off + label_len > msg_len) {
            return 0;
        }
        off += label_len;
    }
}

static int netd_dns_parse_response(const uint8_t* msg, uint32_t msg_len, uint16_t id, uint32_t* out_addr) {
    if (!msg || !out_addr) {
        return 0;
    }

    if (msg_len < sizeof(net_dns_hdr_t)) {
        return 0;
    }

    const net_dns_hdr_t* hdr = (const net_dns_hdr_t*)msg;
    uint16_t rid = netd_ntohs(hdr->id);
    if (rid != id) {
        return 0;
    }

    uint16_t flags = netd_ntohs(hdr->flags);
    if ((flags & 0x8000u) == 0u) {
        return 0;
    }

    if ((flags & 0x000Fu) != 0u) {
        return 0;
    }

    uint16_t qdcount = netd_ntohs(hdr->qdcount);
    uint16_t ancount = netd_ntohs(hdr->ancount);

    uint32_t off = (uint32_t)sizeof(net_dns_hdr_t);

    for (uint16_t i = 0; i < qdcount; i++) {
        if (!netd_dns_skip_name(msg, msg_len, &off)) {
            return 0;
        }
        if (off + 4u > msg_len) {
            return 0;
        }
        off += 4u;
    }

    for (uint16_t i = 0; i < ancount; i++) {
        if (!netd_dns_skip_name(msg, msg_len, &off)) {
            return 0;
        }

        if (off + 10u > msg_len) {
            return 0;
        }

        uint16_t type;
        uint16_t cls;
        uint16_t rdlen;

        memcpy(&type, msg + off, 2u);
        memcpy(&cls, msg + off + 2u, 2u);
        memcpy(&rdlen, msg + off + 8u, 2u);

        type = netd_ntohs(type);
        cls = netd_ntohs(cls);
        rdlen = netd_ntohs(rdlen);

        off += 10u;

        if (off + (uint32_t)rdlen > msg_len) {
            return 0;
        }

        if (type == 1u && cls == 1u && rdlen == 4u) {
            uint32_t addr_n = 0;
            memcpy(&addr_n, msg + off, 4u);
            *out_addr = netd_ntohl(addr_n);
            return 1;
        }

        off += (uint32_t)rdlen;
    }

    return 0;
}

void netd_dns_process_udp(netd_ctx_t* ctx, const net_ipv4_hdr_t* ip, const uint8_t* payload, uint32_t payload_len) {
    if (!ctx || !ip || !payload) {
        return;
    }

    if (payload_len < sizeof(net_udp_hdr_t)) {
        return;
    }

    const net_udp_hdr_t* udp = (const net_udp_hdr_t*)payload;
    uint16_t udp_len = netd_ntohs(udp->len);
    if (udp_len < sizeof(net_udp_hdr_t) || (uint32_t)udp_len > payload_len) {
        return;
    }

    uint16_t src_port = netd_ntohs(udp->src_port);
    uint16_t dst_port = netd_ntohs(udp->dst_port);

    if (src_port != 53u) {
        return;
    }

    uint32_t src_ip = netd_ntohl(ip->src);
    if (ctx->dns_server == 0 || src_ip != ctx->dns_server) {
        return;
    }

    const uint8_t* dns = payload + sizeof(net_udp_hdr_t);
    uint32_t dns_len = (uint32_t)udp_len - (uint32_t)sizeof(net_udp_hdr_t);


    if (ctx->dns_wait.active && !ctx->dns_wait.received && dst_port == ctx->dns_wait.port) {
        uint32_t addr = 0;
        if (netd_dns_parse_response(dns, dns_len, ctx->dns_wait.id, &addr)) {
            ctx->dns_wait.addr = addr;
            ctx->dns_wait.received = 1;
        }
    }

    for (int i = 0; i < NETD_DNS_MAX_WAITS; i++) {
        netd_dns_wait_slot_t* s = &ctx->dns_waits[i];
        if (!s->active || s->received) {
            continue;
        }
        if (dst_port != s->port) {
            continue;
        }
        uint32_t addr = 0;
        if (netd_dns_parse_response(dns, dns_len, s->id, &addr)) {
            s->addr = addr;
            s->received = 1;
        }
    }
}

int netd_dns_query_start(netd_ctx_t* ctx, const char* name, uint32_t timeout_ms) {
    if (!ctx || !ctx->iface.up || !name || !*name) {
        return -1;
    }

    if (ctx->dns_server == 0) {
        return -1;
    }

    if (timeout_ms == 0) {
        timeout_ms = 1000u;
    }

    int h = netd_dns_alloc_slot(ctx);
    if (h < 0) {
        return -1;
    }

    netd_dns_wait_slot_t* s = &ctx->dns_waits[h];
    netd_dns_slot_reset(s);

    uint16_t id = netd_dns_gen_id(ctx);
    uint16_t src_port = netd_dns_port_from_id(id);

    uint8_t query[300];
    uint32_t qlen = netd_dns_build_query(id, name, query, (uint32_t)sizeof(query));
    if (qlen == 0) {
        netd_dns_slot_reset(s);
        return -1;
    }

    s->active = 1;
    s->received = 0;
    s->id = id;
    s->port = src_port;
    s->addr = 0;
    s->start_ms = uptime_ms();
    s->timeout_ms = timeout_ms;

    if (!netd_udp_send(ctx, ctx->dns_server, 53u, src_port, query, qlen)) {
        netd_dns_slot_reset(s);
        return -1;
    }

    return h;
}

int netd_dns_query_poll(netd_ctx_t* ctx, int handle, uint32_t* out_addr, uint32_t* out_status) {
    if (out_status) {
        *out_status = NET_STATUS_ERROR;
    }
    if (out_addr) {
        *out_addr = 0;
    }
    if (!ctx) {
        return 0;
    }

    netd_dns_wait_slot_t* s = netd_dns_slot_by_handle(ctx, handle);
    if (!s || !s->active) {
        return 0;
    }

    if (s->received) {
        if (out_addr) {
            *out_addr = s->addr;
        }
        if (out_status) {
            *out_status = NET_STATUS_OK;
        }
        netd_dns_slot_reset(s);
        return 1;
    }

    uint32_t now_ms = uptime_ms();
    if ((now_ms - s->start_ms) >= s->timeout_ms) {
        if (out_status) {
            *out_status = NET_STATUS_TIMEOUT;
        }
        netd_dns_slot_reset(s);
        return 1;
    }

    if (out_status) {
        *out_status = NET_STATUS_OK;
    }
    return 0;
}

void netd_dns_query_cancel(netd_ctx_t* ctx, int handle) {
    netd_dns_wait_slot_t* s = netd_dns_slot_by_handle(ctx, handle);
    if (!s) {
        return;
    }
    netd_dns_slot_reset(s);
}

int netd_dns_query(netd_ctx_t* ctx, const char* name, uint32_t timeout_ms, uint32_t* out_addr) {
    if (!ctx || !ctx->iface.up || !name || !*name || !out_addr) {
        return 0;
    }

    if (ctx->dns_server == 0) {
        return 0;
    }

    uint16_t id = (uint16_t)(uptime_ms() & 0xFFFFu);
    uint16_t src_port = (uint16_t)(49152u + (uint16_t)(id & 0x03FFu));

    uint8_t query[300];
    uint32_t qlen = netd_dns_build_query(id, name, query, (uint32_t)sizeof(query));
    if (qlen == 0) {
        return 0;
    }

    ctx->dns_wait.active = 1;
    ctx->dns_wait.received = 0;
    ctx->dns_wait.id = id;
    ctx->dns_wait.port = src_port;
    ctx->dns_wait.addr = 0;

    if (!netd_udp_send(ctx, ctx->dns_server, 53u, src_port, query, qlen)) {
        ctx->dns_wait.active = 0;
        return 0;
    }

    uint32_t elapsed = 0;
    uint32_t step_ms = 10;
    if (timeout_ms == 0) {
        timeout_ms = 1000u;
    }

    while (elapsed < timeout_ms) {
        netd_device_process(ctx);

        if (ctx->dns_wait.received) {
            ctx->dns_wait.active = 0;
            *out_addr = ctx->dns_wait.addr;
            return 1;
        }

        sleep((int)step_ms);
        elapsed += step_ms;
    }

    ctx->dns_wait.active = 0;
    return 0;
}
