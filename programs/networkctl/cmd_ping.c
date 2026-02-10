// SPDX-License-Identifier: GPL-2.0

#include "netctl_cmd.h"

#include "netctl_fmt.h"
#include "netctl_ipc.h"
#include "netctl_parse.h"
#include "netctl_print.h"

typedef struct {
    const char* target;
    uint32_t count;
    uint32_t timeout_ms;
} netctl_ping_opts_t;

static int netctl_ping_parse_args(int argc, char** argv, netctl_ping_opts_t* out) {
    if (!out) {
        return -1;
    }

    if (argc < 1) {
        return -1;
    }

    out->target = argv[0];
    out->count = 4;
    out->timeout_ms = 1000;

    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];

        if (strcmp(a, "-c") == 0 && i + 1 < argc) {
            uint32_t tmp = 0;
            if (!netctl_parse_u32(argv[++i], &tmp) || tmp == 0) {
                return -1;
            }
            out->count = tmp;
            continue;
        }

        if (a[0] == '-' && a[1] == 'c' && a[2] != '\0') {
            uint32_t tmp = 0;
            if (!netctl_parse_u32(a + 2, &tmp) || tmp == 0) {
                return -1;
            }
            out->count = tmp;
            continue;
        }

        if (strcmp(a, "-t") == 0 && i + 1 < argc) {
            uint32_t tmp = 0;
            if (!netctl_parse_u32(argv[++i], &tmp) || tmp == 0) {
                return -1;
            }
            out->timeout_ms = tmp;
            continue;
        }

        if (a[0] == '-' && a[1] == 't' && a[2] != '\0') {
            uint32_t tmp = 0;
            if (!netctl_parse_u32(a + 2, &tmp) || tmp == 0) {
                return -1;
            }
            out->timeout_ms = tmp;
            continue;
        }

        return -1;
    }

    return 0;
}

static int netctl_ping_resolve_target(netctl_session_t* s, const char* target, uint32_t timeout_ms, uint32_t* out_ip, int* out_is_ip) {
    if (!s || !target || !*target || !out_ip || !out_is_ip) {
        return -1;
    }

    uint32_t ip = 0;
    int is_ip = netctl_parse_ip4(target, &ip);
    if (!is_ip) {
        if (netctl_dns_query(s, target, timeout_ms, &ip) != 0) {
            return -1;
        }
    }

    *out_ip = ip;
    *out_is_ip = is_ip;
    return 0;
}

static void netctl_ping_print_header(const char* target, uint32_t ip, int is_ip) {
    char ip_buf[32];
    netctl_ip4_to_str(ip, ip_buf, (uint32_t)sizeof(ip_buf));

    if (is_ip) {
        printf("PING %s (%s) 56(84) bytes of data.\n", ip_buf, ip_buf);
        return;
    }

    printf("PING %s (%s) 56(84) bytes of data.\n", target, ip_buf);
}

static int netctl_ping_send_one(netctl_session_t* s, uint32_t dst_ip, uint32_t timeout_ms, uint32_t icmp_seq, net_ping_resp_t* out) {
    if (!s || !out) {
        return -1;
    }

    net_ping_req_t req;
    req.addr = dst_ip;
    req.timeout_ms = timeout_ms;
    req.seq = icmp_seq;

    uint32_t msg_seq = s->seq;
    s->seq = msg_seq + 1u;

    if (net_ipc_send(s->fd_w, NET_IPC_MSG_PING_REQ, msg_seq, &req, (uint32_t)sizeof(req)) != 0) {
        return -1;
    }

    net_ipc_hdr_t hdr;
    if (netctl_wait(
        s->fd_r,
        &s->rx,
        NET_IPC_MSG_PING_RESP,
        msg_seq,
        &hdr,
        (uint8_t*)out,
        (uint32_t)sizeof(*out),
        timeout_ms
    ) != 0) {
        return -2;
    }

    if (hdr.len != (uint32_t)sizeof(*out)) {
        return -1;
    }

    return 0;
}

static void netctl_ping_print_reply(const char* ip_str, const net_ping_resp_t* resp, uint32_t icmp_seq) {
    if (!ip_str || !resp) {
        return;
    }

    if (resp->status == NET_STATUS_OK) {
        printf("64 bytes from %s: icmp_seq=%u time=%ums\n", ip_str, resp->seq, resp->rtt_ms);
        return;
    }

    if (resp->status == NET_STATUS_UNREACHABLE) {
        printf("From %s icmp_seq=%u Destination Host Unreachable\n", ip_str, resp->seq);
        return;
    }

    if (resp->status == NET_STATUS_TIMEOUT) {
        printf("Request timeout for icmp_seq=%u\n", resp->seq);
        return;
    }

    printf("From %s icmp_seq=%u Error\n", ip_str, icmp_seq);
}

int netctl_cmd_ping(int argc, char** argv) {
    netctl_ping_opts_t opts;
    if (netctl_ping_parse_args(argc, argv, &opts) != 0) {
        netctl_print_usage();
        return 1;
    }

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

    uint32_t dst_ip = 0;
    int is_ip = 0;
    if (netctl_ping_resolve_target(&s, opts.target, opts.timeout_ms, &dst_ip, &is_ip) != 0) {
        printf("ping: cannot resolve %s\n", opts.target);
        netctl_session_close(&s);
        return 1;
    }

    netctl_ping_print_header(opts.target, dst_ip, is_ip);

    char ip_buf[32];
    netctl_ip4_to_str(dst_ip, ip_buf, (uint32_t)sizeof(ip_buf));

    uint32_t transmitted = 0;
    uint32_t received = 0;
    uint32_t unreachable = 0;
    uint32_t timeouts = 0;
    uint32_t t_start = uptime_ms();

    for (uint32_t i = 0; i < opts.count; i++) {
        uint32_t icmp_seq = i + 1u;
        transmitted++;

        net_ping_resp_t resp;
        int rc = netctl_ping_send_one(&s, dst_ip, opts.timeout_ms, icmp_seq, &resp);
        if (rc == -2) {
            printf("Request timeout for icmp_seq=%u\n", icmp_seq);
            timeouts++;
            continue;
        }

        if (rc != 0) {
            printf("Request timeout for icmp_seq=%u\n", icmp_seq);
            timeouts++;
            continue;
        }

        if (resp.status == NET_STATUS_OK) {
            received++;
        } else if (resp.status == NET_STATUS_UNREACHABLE) {
            unreachable++;
        } else if (resp.status == NET_STATUS_TIMEOUT) {
            timeouts++;
        }

        netctl_ping_print_reply(ip_buf, &resp, icmp_seq);
    }

    uint32_t t_end = uptime_ms();
    uint32_t loss = 0;
    if (transmitted > 0) {
        uint32_t lost = transmitted - received;
        loss = (lost * 100u) / transmitted;
    }

    uint32_t total_ms = t_end - t_start;
    printf("--- %s ping statistics ---\n", ip_buf);
    printf("%u packets transmitted, %u received, %u%% packet loss, time %ums\n", transmitted, received, loss, total_ms);

    if (unreachable > 0) {
        printf("%u unreachable\n", unreachable);
    }

    netctl_session_close(&s);
    return (received > 0) ? 0 : 1;
}

