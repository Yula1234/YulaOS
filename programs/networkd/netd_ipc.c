// SPDX-License-Identifier: GPL-2.0

#include "netd_ipc.h"

#include <yula.h>

#include "netd_dns.h"
#include "netd_iface.h"
#include "netd_ipv4.h"

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

    uint32_t addr = 0;
    if (netd_dns_query(ctx, req->name, req->timeout_ms, &addr)) {
        resp.status = NET_STATUS_OK;
        resp.addr = addr;
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

    if (hdr->type == NET_IPC_MSG_HELLO) {
        (void)netd_send_status(ctx, c->fd_out, hdr->seq, NET_STATUS_OK);
        return;
    }

    if (hdr->type == NET_IPC_MSG_STATUS_REQ) {
        (void)netd_send_status(ctx, c->fd_out, hdr->seq, NET_STATUS_OK);
        return;
    }

    if (hdr->type == NET_IPC_MSG_LINK_LIST_REQ) {
        (void)netd_send_link_list(ctx, c->fd_out, hdr->seq);
        return;
    }

    if (hdr->type == NET_IPC_MSG_PING_REQ && hdr->len == (uint32_t)sizeof(net_ping_req_t)) {
        net_ping_req_t req;
        memcpy(&req, payload, sizeof(req));
        (void)netd_send_ping_resp(ctx, c->fd_out, hdr->seq, &req);
        return;
    }

    if (hdr->type == NET_IPC_MSG_DNS_REQ && hdr->len == (uint32_t)sizeof(net_dns_req_t)) {
        net_dns_req_t req;
        memcpy(&req, payload, sizeof(req));
        req.name[(uint32_t)sizeof(req.name) - 1u] = '\0';
        (void)netd_send_dns_resp(ctx, c->fd_out, hdr->seq, &req);
        return;
    }

    if (hdr->type == NET_IPC_MSG_CFG_GET_REQ && hdr->len == 0u) {
        (void)netd_send_cfg_resp(ctx, c->fd_out, NET_IPC_MSG_CFG_GET_RESP, hdr->seq, NET_STATUS_OK);
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
        (void)netd_send_cfg_resp(ctx, c->fd_out, NET_IPC_MSG_CFG_SET_RESP, hdr->seq, NET_STATUS_OK);
        return;
    }

    if (hdr->type == NET_IPC_MSG_IFACE_UP_REQ && hdr->len == 0u) {
        uint32_t st = NET_STATUS_OK;
        if (netd_iface_ensure_up(ctx) != 0) {
            st = NET_STATUS_UNREACHABLE;
        }
        netd_links_init(ctx);
        (void)netd_send_iface_resp(ctx, c->fd_out, NET_IPC_MSG_IFACE_UP_RESP, hdr->seq, st);
        return;
    }

    if (hdr->type == NET_IPC_MSG_IFACE_DOWN_REQ && hdr->len == 0u) {
        netd_iface_close(ctx);
        netd_links_init(ctx);
        (void)netd_send_iface_resp(ctx, c->fd_out, NET_IPC_MSG_IFACE_DOWN_RESP, hdr->seq, NET_STATUS_OK);
        return;
    }
}

void netd_ipc_clients_init(netd_client_t clients[NETD_MAX_CLIENTS]) {
    if (!clients) {
        return;
    }

    memset(clients, 0, sizeof(netd_client_t) * NETD_MAX_CLIENTS);
    for (int i = 0; i < NETD_MAX_CLIENTS; i++) {
        clients[i].fd_in = -1;
        clients[i].fd_out = -1;
    }
}

void netd_ipc_accept_pending(int listen_fd, netd_client_t clients[NETD_MAX_CLIENTS]) {
    if (!clients) {
        return;
    }

    for (;;) {
        int out_fds[2];
        int acc = ipc_accept(listen_fd, out_fds);
        if (acc <= 0) {
            break;
        }

        int slot = -1;
        for (int i = 0; i < NETD_MAX_CLIENTS; i++) {
            if (!clients[i].used) {
                slot = i;
                break;
            }
        }

        if (slot < 0) {
            close(out_fds[0]);
            close(out_fds[1]);
            continue;
        }

        netd_client_t* c = &clients[slot];
        c->used = 1;
        c->fd_in = out_fds[0];
        c->fd_out = out_fds[1];
        net_ipc_rx_reset(&c->rx);
    }
}

void netd_ipc_process_clients(netd_ctx_t* ctx, netd_client_t clients[NETD_MAX_CLIENTS]) {
    if (!ctx || !clients) {
        return;
    }

    for (int i = 0; i < NETD_MAX_CLIENTS; i++) {
        netd_client_t* c = &clients[i];
        if (!c->used) {
            continue;
        }

        net_ipc_hdr_t hdr;
        uint8_t payload[NET_IPC_MAX_PAYLOAD];

        for (;;) {
            int r = net_ipc_try_recv(&c->rx, c->fd_in, &hdr, payload, (uint32_t)sizeof(payload));
            if (r < 0) {
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
