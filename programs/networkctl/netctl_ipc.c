// SPDX-License-Identifier: GPL-2.0

#include "netctl_ipc.h"

static int netctl_connect(int* out_r, int* out_w) {
    if (!out_r || !out_w) {
        return -1;
    }

    int fds[2];
    if (ipc_connect("networkd", fds) != 0) {
        return -1;
    }

    *out_r = fds[0];
    *out_w = fds[1];
    return 0;
}

static void netctl_close_fds(int fd_r, int fd_w) {
    if (fd_r >= 0) {
        close(fd_r);
    }
    if (fd_w >= 0 && fd_w != fd_r) {
        close(fd_w);
    }
}

int netctl_session_open(netctl_session_t* s) {
    if (!s) {
        return -1;
    }

    s->fd_r = -1;
    s->fd_w = -1;
    s->seq = 1;
    net_ipc_rx_reset(&s->rx);

    if (netctl_connect(&s->fd_r, &s->fd_w) != 0) {
        return -1;
    }

    return 0;
}

void netctl_session_close(netctl_session_t* s) {
    if (!s) {
        return;
    }

    netctl_close_fds(s->fd_r, s->fd_w);
    s->fd_r = -1;
    s->fd_w = -1;
}

int netctl_session_send_hello(netctl_session_t* s) {
    if (!s) {
        return -1;
    }

    uint32_t seq = s->seq;
    if (net_ipc_send(s->fd_w, NET_IPC_MSG_HELLO, seq, 0, 0) != 0) {
        return -1;
    }
    s->seq = seq + 1u;
    return 0;
}

int netctl_wait(
    int fd,
    net_ipc_rx_t* rx,
    uint16_t want_type,
    uint32_t want_seq,
    net_ipc_hdr_t* out_hdr,
    uint8_t* out_payload,
    uint32_t cap,
    uint32_t timeout_ms
) {
    uint32_t start = uptime_ms();
    for (;;) {
        net_ipc_hdr_t hdr;
        uint8_t payload[NET_IPC_MAX_PAYLOAD];

        int r = net_ipc_try_recv(rx, fd, &hdr, payload, (uint32_t)sizeof(payload));
        if (r < 0) {
            return -1;
        }

        if (r > 0) {
            if (hdr.type == want_type && (want_seq == 0u || hdr.seq == want_seq)) {
                if (out_hdr) {
                    *out_hdr = hdr;
                }

                if (out_payload && hdr.len > 0) {
                    uint32_t copy_len = hdr.len;
                    if (copy_len > cap) {
                        copy_len = cap;
                    }
                    memcpy(out_payload, payload, copy_len);
                }

                return 0;
            }
        }

        uint32_t now = uptime_ms();
        if ((now - start) >= timeout_ms) {
            return -1;
        }

        pollfd_t pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        (void)poll(&pfd, 1, 50);
    }
}

int netctl_dns_query(netctl_session_t* s, const char* name, uint32_t timeout_ms, uint32_t* out_addr) {
    if (!s || !name || !*name || !out_addr) {
        return -1;
    }

    net_dns_req_t req;
    memset(&req, 0, sizeof(req));
    req.timeout_ms = timeout_ms;
    strncpy(req.name, name, (uint32_t)sizeof(req.name) - 1u);

    uint32_t msg_seq = s->seq;
    s->seq = msg_seq + 1u;

    if (net_ipc_send(s->fd_w, NET_IPC_MSG_DNS_REQ, msg_seq, &req, (uint32_t)sizeof(req)) != 0) {
        return -1;
    }

    net_ipc_hdr_t hdr;
    net_dns_resp_t resp;
    if (netctl_wait(
        s->fd_r,
        &s->rx,
        NET_IPC_MSG_DNS_RESP,
        msg_seq,
        &hdr,
        (uint8_t*)&resp,
        (uint32_t)sizeof(resp),
        timeout_ms
    ) != 0) {
        return -1;
    }

    if (hdr.len != (uint32_t)sizeof(resp)) {
        return -1;
    }

    if (resp.status != NET_STATUS_OK || resp.addr == 0) {
        return -1;
    }

    *out_addr = resp.addr;
    return 0;
}

