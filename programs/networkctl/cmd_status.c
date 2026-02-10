// SPDX-License-Identifier: GPL-2.0

#include "netctl_cmd.h"

#include "netctl_ipc.h"
#include "netctl_print.h"

static int netctl_fetch_status(netctl_session_t* s, net_status_resp_t* out) {
    if (!s || !out) {
        return -1;
    }

    uint32_t msg_seq = s->seq;
    s->seq = msg_seq + 1u;

    if (net_ipc_send(s->fd_w, NET_IPC_MSG_STATUS_REQ, msg_seq, 0, 0) != 0) {
        return -1;
    }

    net_ipc_hdr_t hdr;
    if (netctl_wait(
        s->fd_r,
        &s->rx,
        NET_IPC_MSG_STATUS_RESP,
        msg_seq,
        &hdr,
        (uint8_t*)out,
        (uint32_t)sizeof(*out),
        1000
    ) != 0) {
        return -1;
    }

    if (hdr.len != (uint32_t)sizeof(*out)) {
        return -1;
    }

    return 0;
}

static void netctl_print_status(const net_status_resp_t* st) {
    if (!st) {
        printf("state: unknown\n");
        return;
    }

    const char* state = (st->status == NET_STATUS_OK) ? "running" : "error";
    printf("state: %s\n", state);
    printf("links: %u\n", st->link_count);
}

static void netctl_try_print_cfg(netctl_session_t* s) {
    if (!s) {
        return;
    }

    uint32_t msg_seq = s->seq;
    s->seq = msg_seq + 1u;

    if (net_ipc_send(s->fd_w, NET_IPC_MSG_CFG_GET_REQ, msg_seq, 0, 0) != 0) {
        return;
    }

    net_ipc_hdr_t hdr;
    net_cfg_resp_t cfg;
    if (netctl_wait(
        s->fd_r,
        &s->rx,
        NET_IPC_MSG_CFG_GET_RESP,
        msg_seq,
        &hdr,
        (uint8_t*)&cfg,
        (uint32_t)sizeof(cfg),
        1000
    ) != 0) {
        return;
    }

    if (hdr.len != (uint32_t)sizeof(cfg)) {
        return;
    }

    if (cfg.status != NET_STATUS_OK) {
        return;
    }

    netctl_print_cfg(&cfg);
}

static int netctl_fetch_links(netctl_session_t* s, uint8_t* payload, uint32_t cap, uint32_t* out_len) {
    if (!s || !payload || cap == 0 || !out_len) {
        return -1;
    }

    uint32_t msg_seq = s->seq;
    s->seq = msg_seq + 1u;

    if (net_ipc_send(s->fd_w, NET_IPC_MSG_LINK_LIST_REQ, msg_seq, 0, 0) != 0) {
        return -1;
    }

    net_ipc_hdr_t hdr;
    if (netctl_wait(
        s->fd_r,
        &s->rx,
        NET_IPC_MSG_LINK_LIST_RESP,
        msg_seq,
        &hdr,
        payload,
        cap,
        1000
    ) != 0) {
        return -1;
    }

    *out_len = hdr.len;
    return 0;
}

int netctl_cmd_status(int show_links) {
    netctl_session_t s;
    if (netctl_session_open(&s) != 0) {
        printf("networkctl: cannot connect to networkd\n");
        return 1;
    }

    if (netctl_session_send_hello(&s) != 0) {
        netctl_session_close(&s);
        printf("networkctl: cannot connect to networkd\n");
        return 1;
    }

    net_status_resp_t st;
    if (netctl_fetch_status(&s, &st) == 0) {
        netctl_print_status(&st);
    } else {
        printf("state: unknown\n");
    }

    netctl_try_print_cfg(&s);

    if (show_links) {
        uint8_t payload[NET_IPC_MAX_PAYLOAD];
        uint32_t payload_len = 0;

        if (netctl_fetch_links(&s, payload, (uint32_t)sizeof(payload), &payload_len) == 0) {
            netctl_print_links(payload, payload_len);
        } else {
            printf("links: not available\n");
        }
    }

    netctl_session_close(&s);
    return 0;
}

int netctl_cmd_links(void) {
    netctl_session_t s;
    if (netctl_session_open(&s) != 0) {
        printf("networkctl: cannot connect to networkd\n");
        return 1;
    }

    if (netctl_session_send_hello(&s) != 0) {
        netctl_session_close(&s);
        printf("networkctl: cannot connect to networkd\n");
        return 1;
    }

    uint8_t payload[NET_IPC_MAX_PAYLOAD];
    uint32_t payload_len = 0;
    if (netctl_fetch_links(&s, payload, (uint32_t)sizeof(payload), &payload_len) != 0) {
        printf("links: not available\n");
        netctl_session_close(&s);
        return 1;
    }

    netctl_print_links(payload, payload_len);
    netctl_session_close(&s);
    return 0;
}

