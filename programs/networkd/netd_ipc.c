// SPDX-License-Identifier: GPL-2.0

#include "netd_ipc.h"

#include <yula.h>

#include "netd_config.h"
#include "netd_dns.h"
#include "netd_dns_cache.h"
#include "netd_http.h"
#include "netd_iface.h"
#include "netd_ipv4.h"
#include "netd_stats.h"

static int netd_ipc_is_https_url(const char* url) {
    if (!url) {
        return 0;
    }
    return strncmp(url, "https://", 8u) == 0;
}

static void netd_close_client(netd_client_t* c) {
    if (!c || !c->used) {
        return;
    }

    if (c->fd_in >= 0) {
        close(c->fd_in);
    }

    if (c->fd_out >= 0 && c->fd_out != c->fd_in) {
        close(c->fd_out);
    }

    c->used = 0;
    c->fd_in = -1;
    c->fd_out = -1;
    net_ipc_rx_reset(&c->rx);
}

static int netd_send_status(netd_ctx_t* ctx, int fd, uint32_t seq, uint32_t status) {
    net_status_resp_t resp;
    resp.status = status;
    resp.link_count = ctx ? ctx->state.count : 0;
    resp.flags = 0;
    return net_ipc_send(fd, NET_IPC_MSG_STATUS_RESP, seq, &resp, (uint32_t)sizeof(resp));
}

static int netd_send_link_list(netd_ctx_t* ctx, int fd, uint32_t seq) {
    if (!ctx) {
        return -1;
    }

    uint8_t payload[NET_IPC_MAX_PAYLOAD];

    if (ctx->state.count > 4) {
        ctx->state.count = 4;
    }

    net_link_list_hdr_t hdr;
    hdr.count = ctx->state.count;

    uint32_t off = 0;
    memcpy(payload + off, &hdr, (uint32_t)sizeof(hdr));
    off += (uint32_t)sizeof(hdr);

    for (uint32_t i = 0; i < ctx->state.count; i++) {
        memcpy(payload + off, &ctx->state.links[i], (uint32_t)sizeof(net_link_info_t));
        off += (uint32_t)sizeof(net_link_info_t);
    }

    return net_ipc_send(fd, NET_IPC_MSG_LINK_LIST_RESP, seq, payload, off);
}

static int netd_send_ping_resp(netd_ctx_t* ctx, int fd, uint32_t seq, const net_ping_req_t* req) {
    if (!ctx || !req) {
        return -1;
    }

    net_ping_resp_t resp;
    resp.addr = req->addr;
    resp.seq = req->seq;
    resp.status = NET_STATUS_TIMEOUT;
    resp.rtt_ms = 0;

    if (req->addr == 0x7F000001u) {
        resp.status = NET_STATUS_OK;
        resp.rtt_ms = 1;
    } else if (!ctx->iface.up) {
        resp.status = NET_STATUS_UNREACHABLE;
    } else {
        uint32_t rtt_ms = 0;
        resp.status = netd_ipv4_send_ping(ctx, req->addr, req->timeout_ms, (uint16_t)req->seq, &rtt_ms);
        resp.rtt_ms = rtt_ms;
    }

    return net_ipc_send(fd, NET_IPC_MSG_PING_RESP, seq, &resp, (uint32_t)sizeof(resp));
}

static int netd_send_dns_resp(netd_ctx_t* ctx, int fd, uint32_t seq, const net_dns_req_t* req) {
    if (!ctx || !req) {
        return -1;
    }

    net_dns_resp_t resp;
    resp.status = NET_STATUS_TIMEOUT;
    resp.addr = 0;

    if (!ctx->iface.up) {
        resp.status = NET_STATUS_UNREACHABLE;
        return net_ipc_send(fd, NET_IPC_MSG_DNS_RESP, seq, &resp, (uint32_t)sizeof(resp));
    }

    if (ctx->enable_stats) {
        netd_stats_dns_query(&ctx->stats);
    }

    uint32_t addr = 0;
    if (netd_dns_cache_lookup(&ctx->dns_cache, req->name, &addr)) {
        if (ctx->enable_stats) {
            netd_stats_dns_cache_hit(&ctx->stats);
        }
        resp.status = NET_STATUS_OK;
        resp.addr = addr;
        return net_ipc_send(fd, NET_IPC_MSG_DNS_RESP, seq, &resp, (uint32_t)sizeof(resp));
    }

    if (ctx->enable_stats) {
        netd_stats_dns_cache_miss(&ctx->stats);
    }

    if (netd_dns_query(ctx, req->name, req->timeout_ms, &addr)) {
        netd_dns_cache_insert(&ctx->dns_cache, req->name, addr, 0);
        if (ctx->enable_stats) {
            netd_stats_dns_response(&ctx->stats);
        }
        resp.status = NET_STATUS_OK;
        resp.addr = addr;
    } else {
        if (ctx->enable_stats) {
            netd_stats_dns_timeout(&ctx->stats);
        }
    }

    return net_ipc_send(fd, NET_IPC_MSG_DNS_RESP, seq, &resp, (uint32_t)sizeof(resp));
}



static int netd_send_cfg_resp(netd_ctx_t* ctx, int fd, uint16_t type, uint32_t seq, uint32_t status) {
    if (!ctx) {
        return -1;
    }

    net_cfg_resp_t resp;
    resp.status = status;
    resp.ip = ctx->iface.ip;
    resp.mask = ctx->iface.mask;
    resp.gw = ctx->iface.gw;
    resp.dns = ctx->dns_server;
    return net_ipc_send(fd, type, seq, &resp, (uint32_t)sizeof(resp));
}

static int netd_send_iface_resp(netd_ctx_t* ctx, int fd, uint16_t type, uint32_t seq, uint32_t status) {
    net_status_resp_t resp;
    resp.status = status;
    resp.link_count = ctx ? ctx->state.count : 0;
    resp.flags = 0;
    return net_ipc_send(fd, type, seq, &resp, (uint32_t)sizeof(resp));
}

static void netd_handle_msg(netd_ctx_t* ctx, netd_client_t* c, const net_ipc_hdr_t* hdr, const uint8_t* payload) {
    if (!ctx || !c || !hdr) {
        return;
    }
    
    c->req_count++;
    c->last_activity_ms = uptime_ms();

    if (hdr->type == NET_IPC_MSG_HELLO) {
        netd_send_status(ctx, c->fd_out, hdr->seq, NET_STATUS_OK);
        return;
    }

    if (hdr->type == NET_IPC_MSG_STATUS_REQ) {
        netd_send_status(ctx, c->fd_out, hdr->seq, NET_STATUS_OK);
        return;
    }

    if (hdr->type == NET_IPC_MSG_LINK_LIST_REQ) {
        netd_send_link_list(ctx, c->fd_out, hdr->seq);
        return;
    }

    if (hdr->type == NET_IPC_MSG_PING_REQ && hdr->len == (uint32_t)sizeof(net_ping_req_t)) {
        net_ping_req_t req;
        memcpy(&req, payload, sizeof(req));
        netd_send_ping_resp(ctx, c->fd_out, hdr->seq, &req);
        return;
    }

    if (hdr->type == NET_IPC_MSG_DNS_REQ && hdr->len == (uint32_t)sizeof(net_dns_req_t)) {
        net_dns_req_t req;
        memcpy(&req, payload, sizeof(req));
        req.name[(uint32_t)sizeof(req.name) - 1u] = '\0';
        netd_send_dns_resp(ctx, c->fd_out, hdr->seq, &req);
        return;
    }

    if (hdr->type == NET_IPC_MSG_CFG_GET_REQ && hdr->len == 0u) {
        netd_send_cfg_resp(ctx, c->fd_out, NET_IPC_MSG_CFG_GET_RESP, hdr->seq, NET_STATUS_OK);
        return;
    }

    if (hdr->type == NET_IPC_MSG_CFG_SET_REQ && hdr->len == (uint32_t)sizeof(net_cfg_set_t)) {
        net_cfg_set_t req;
        memcpy(&req, payload, sizeof(req));

        if ((req.flags & NET_CFG_F_IP) != 0u) {
            ctx->iface.ip = req.ip;
        }

        if ((req.flags & NET_CFG_F_MASK) != 0u) {
            ctx->iface.mask = req.mask;
        }

        if ((req.flags & NET_CFG_F_GW) != 0u) {
            ctx->iface.gw = req.gw;
        }

        if ((req.flags & NET_CFG_F_DNS) != 0u) {
            ctx->dns_server = req.dns;
        }

        netd_links_init(ctx);
        netd_send_cfg_resp(ctx, c->fd_out, NET_IPC_MSG_CFG_SET_RESP, hdr->seq, NET_STATUS_OK);
        return;
    }

    if (hdr->type == NET_IPC_MSG_IFACE_UP_REQ && hdr->len == 0u) {
        uint32_t st = NET_STATUS_OK;
        if (netd_iface_ensure_up(ctx) != 0) {
            st = NET_STATUS_UNREACHABLE;
        }
        netd_links_init(ctx);
        netd_send_iface_resp(ctx, c->fd_out, NET_IPC_MSG_IFACE_UP_RESP, hdr->seq, st);
        return;
    }

    if (hdr->type == NET_IPC_MSG_IFACE_DOWN_REQ && hdr->len == 0u) {
        netd_iface_close(ctx);
        netd_links_init(ctx);
        netd_send_iface_resp(ctx, c->fd_out, NET_IPC_MSG_IFACE_DOWN_RESP, hdr->seq, NET_STATUS_OK);
        return;
    }

    if (hdr->type == NET_IPC_MSG_HTTP_GET_REQ && hdr->len == (uint32_t)sizeof(net_http_get_req_t)) {
        net_http_get_req_t req;
        memcpy(&req, payload, sizeof(req));
        req.url[(uint32_t)sizeof(req.url) - 1u] = '\0';
        if (netd_ipc_is_https_url(req.url)) {
            netd_http_get(ctx, c->fd_out, hdr->seq, &req);
        } else {
            netd_http_get_start(ctx, c->fd_out, hdr->seq, &req);
        }
        return;
    }
}



void netd_ipc_clients_init(netd_client_t* clients, uint32_t capacity) {
    if (!clients || capacity == 0) {
        return;
    }

    memset(clients, 0, sizeof(netd_client_t) * capacity);
    for (uint32_t i = 0; i < capacity; i++) {
        clients[i].fd_in = -1;
        clients[i].fd_out = -1;
        clients[i].used = 0;
        clients[i].req_count = 0;
        clients[i].last_activity_ms = 0;
    }
}

void netd_ipc_accept_pending(netd_ctx_t* ctx, int listen_fd) {
    if (!ctx) {
        return;
    }
    
    netd_log_debug(ctx, "netd_ipc_accept_pending called (deprecated function)");
}

void netd_ipc_process_clients(netd_ctx_t* ctx, netd_client_t* clients, uint32_t count) {
    if (!ctx || !clients) {
        return;
    }

    for (uint32_t i = 0; i < count; i++) {
        netd_client_t* c = &clients[i];
        if (!c->used) {
            continue;
        }

        net_ipc_hdr_t hdr;
        uint8_t payload[NET_IPC_MAX_PAYLOAD];

        for (;;) {
            int r = net_ipc_try_recv(&c->rx, c->fd_in, &hdr, payload, (uint32_t)sizeof(payload));
            if (r < 0) {
                netd_log_debug(ctx, "IPC client disconnected (slot %u)", i);
                netd_close_client(c);
                break;
            }
            if (r == 0) {
                break;
            }
            netd_handle_msg(ctx, c, &hdr, payload);
        }
    }
}
