// SPDX-License-Identifier: GPL-2.0

#include "netctl_cmd.h"

#include "netctl_ipc.h"

static int netctl_iface_send(netctl_session_t* s, int up) {
    if (!s) {
        return -1;
    }

    uint16_t req_type = up ? NET_IPC_MSG_IFACE_UP_REQ : NET_IPC_MSG_IFACE_DOWN_REQ;
    uint16_t resp_type = up ? NET_IPC_MSG_IFACE_UP_RESP : NET_IPC_MSG_IFACE_DOWN_RESP;

    uint32_t msg_seq = s->seq;
    s->seq = msg_seq + 1u;

    if (net_ipc_send(s->fd_w, req_type, msg_seq, 0, 0) != 0) {
        return -1;
    }

    net_ipc_hdr_t hdr;
    net_status_resp_t resp;
    if (netctl_wait(
        s->fd_r,
        &s->rx,
        resp_type,
        msg_seq,
        &hdr,
        (uint8_t*)&resp,
        (uint32_t)sizeof(resp),
        1000
    ) != 0) {
        return -1;
    }

    if (hdr.len != (uint32_t)sizeof(resp)) {
        return -1;
    }

    if (resp.status != NET_STATUS_OK) {
        return -1;
    }

    return 0;
}

int netctl_cmd_iface(int up) {
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

    if (netctl_iface_send(&s, up) != 0) {
        printf("iface: error\n");
        netctl_session_close(&s);
        return 1;
    }

    printf("iface: %s\n", up ? "up" : "down");
    netctl_session_close(&s);
    return 0;
}

