// SPDX-License-Identifier: GPL-2.0

#include "netctl_cmd.h"

#include "netctl_ipc.h"
#include "netctl_parse.h"
#include "netctl_print.h"

static int netctl_cfg_get(netctl_session_t* s, net_cfg_resp_t* out) {
    if (!s || !out) {
        return -1;
    }

    uint32_t msg_seq = s->seq;
    s->seq = msg_seq + 1u;

    if (net_ipc_send(s->fd_w, NET_IPC_MSG_CFG_GET_REQ, msg_seq, 0, 0) != 0) {
        return -1;
    }

    net_ipc_hdr_t hdr;
    if (netctl_wait(
        s->fd_r,
        &s->rx,
        NET_IPC_MSG_CFG_GET_RESP,
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

static int netctl_cfg_set(netctl_session_t* s, const net_cfg_set_t* req, net_cfg_resp_t* out) {
    if (!s || !req || !out) {
        return -1;
    }

    uint32_t msg_seq = s->seq;
    s->seq = msg_seq + 1u;

    if (net_ipc_send(s->fd_w, NET_IPC_MSG_CFG_SET_REQ, msg_seq, req, (uint32_t)sizeof(*req)) != 0) {
        return -1;
    }

    net_ipc_hdr_t hdr;
    if (netctl_wait(
        s->fd_r,
        &s->rx,
        NET_IPC_MSG_CFG_SET_RESP,
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

static int netctl_cfg_parse_set(int argc, char** argv, net_cfg_set_t* out) {
    if (!out) {
        return -1;
    }

    memset(out, 0, sizeof(*out));

    for (int i = 0; i < argc; i++) {
        const char* key = argv[i];

        if (strcmp(key, "ip") == 0 && i + 1 < argc) {
            uint32_t v = 0;
            if (!netctl_parse_ip4(argv[++i], &v)) {
                return -1;
            }
            out->flags |= NET_CFG_F_IP;
            out->ip = v;
            continue;
        }

        if (strcmp(key, "mask") == 0 && i + 1 < argc) {
            uint32_t v = 0;
            if (!netctl_parse_ip4(argv[++i], &v)) {
                return -1;
            }
            out->flags |= NET_CFG_F_MASK;
            out->mask = v;
            continue;
        }

        if (strcmp(key, "gw") == 0 && i + 1 < argc) {
            uint32_t v = 0;
            if (!netctl_parse_ip4(argv[++i], &v)) {
                return -1;
            }
            out->flags |= NET_CFG_F_GW;
            out->gw = v;
            continue;
        }

        if (strcmp(key, "dns") == 0 && i + 1 < argc) {
            uint32_t v = 0;
            if (!netctl_parse_ip4(argv[++i], &v)) {
                return -1;
            }
            out->flags |= NET_CFG_F_DNS;
            out->dns = v;
            continue;
        }

        return -1;
    }

    if (out->flags == 0u) {
        return -1;
    }

    return 0;
}

int netctl_cmd_config(int argc, char** argv) {
    if (argc < 1) {
        netctl_print_usage();
        return 1;
    }

    const char* sub = argv[0];

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

    if (strcmp(sub, "show") == 0) {
        net_cfg_resp_t resp;
        if (netctl_cfg_get(&s, &resp) != 0) {
            printf("config: not available\n");
            netctl_session_close(&s);
            return 1;
        }

        if (resp.status != NET_STATUS_OK) {
            printf("config: error\n");
            netctl_session_close(&s);
            return 1;
        }

        netctl_print_cfg(&resp);
        netctl_session_close(&s);
        return 0;
    }

    if (strcmp(sub, "set") == 0) {
        net_cfg_set_t req;
        if (netctl_cfg_parse_set(argc - 1, &argv[1], &req) != 0) {
            netctl_print_usage();
            netctl_session_close(&s);
            return 1;
        }

        net_cfg_resp_t resp;
        if (netctl_cfg_set(&s, &req, &resp) != 0) {
            printf("config: set failed\n");
            netctl_session_close(&s);
            return 1;
        }

        if (resp.status != NET_STATUS_OK) {
            printf("config: set error\n");
            netctl_session_close(&s);
            return 1;
        }

        netctl_print_cfg(&resp);
        netctl_session_close(&s);
        return 0;
    }

    netctl_print_usage();
    netctl_session_close(&s);
    return 1;
}

