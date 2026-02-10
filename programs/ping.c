// SPDX-License-Identifier: GPL-2.0

#include <yula.h>
#include <net_ipc.h>

static int parse_ip4(const char* s, uint32_t* out) {
    if (!s || !out) return 0;

    uint32_t parts[4];
    uint32_t count = 0;
    const char* p = s;

    while (*p && count < 4) {
        if (*p < '0' || *p > '9') return 0;
        uint32_t val = 0;
        while (*p >= '0' && *p <= '9') {
            val = (val * 10u) + (uint32_t)(*p - '0');
            if (val > 255u) return 0;
            p++;
        }
        parts[count++] = val;
        if (*p == '.') p++;
        else break;
    }

    if (count != 4 || *p != '\0') return 0;

    *out = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
    return 1;
}

static void ip4_to_str(uint32_t addr, char* out, uint32_t cap) {
    if (!out || cap == 0) return;
    uint8_t a = (uint8_t)((addr >> 24) & 0xFFu);
    uint8_t b = (uint8_t)((addr >> 16) & 0xFFu);
    uint8_t c = (uint8_t)((addr >> 8) & 0xFFu);
    uint8_t d = (uint8_t)(addr & 0xFFu);
    snprintf(out, cap, "%u.%u.%u.%u", a, b, c, d);
}

static int ping_wait(int fd, net_ipc_rx_t* rx, uint32_t want_seq, net_ping_resp_t* out, uint32_t timeout_ms) {
    uint32_t start = uptime_ms();

    for (;;) {
        net_ipc_hdr_t hdr;
        uint8_t payload[NET_IPC_MAX_PAYLOAD];
        int r = net_ipc_try_recv(rx, fd, &hdr, payload, (uint32_t)sizeof(payload));
        if (r < 0) return -1;
        if (r > 0) {
            if (hdr.type == NET_IPC_MSG_PING_RESP && hdr.len == (uint32_t)sizeof(net_ping_resp_t)) {
                net_ping_resp_t resp;
                memcpy(&resp, payload, sizeof(resp));
                if (resp.seq == want_seq) {
                    if (out) *out = resp;
                    return 0;
                }
            }
        }

        uint32_t now = uptime_ms();
        if (now - start >= timeout_ms) return -1;

        pollfd_t pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        (void)poll(&pfd, 1, 50);
    }
}

static int dns_wait(int fd, net_ipc_rx_t* rx, uint32_t want_ipc_seq, net_dns_resp_t* out, uint32_t timeout_ms) {
    uint32_t start = uptime_ms();

    for (;;) {
        net_ipc_hdr_t hdr;
        uint8_t payload[NET_IPC_MAX_PAYLOAD];
        int r = net_ipc_try_recv(rx, fd, &hdr, payload, (uint32_t)sizeof(payload));
        if (r < 0) return -1;
        if (r > 0) {
            if (hdr.seq == want_ipc_seq && hdr.type == NET_IPC_MSG_DNS_RESP && hdr.len == (uint32_t)sizeof(net_dns_resp_t)) {
                net_dns_resp_t resp;
                memcpy(&resp, payload, sizeof(resp));
                if (out) *out = resp;
                return 0;
            }
        }

        uint32_t now = uptime_ms();
        if (now - start >= timeout_ms) return -1;

        pollfd_t pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        (void)poll(&pfd, 1, 50);
    }
}

static int connect_networkd(int* out_r, int* out_w) {
    int fds[2];
    if (ipc_connect("networkd", fds) != 0) {
        return -1;
    }
    *out_r = fds[0];
    *out_w = fds[1];
    return 0;
}

static int parse_uint32(const char* s, uint32_t* out) {
    if (!s || !*s || !out) return 0;
    uint32_t v = 0;
    const char* p = s;
    while (*p) {
        if (*p < '0' || *p > '9') return 0;
        v = (v * 10u) + (uint32_t)(*p - '0');
        p++;
    }
    *out = v;
    return 1;
}

static void print_usage(void) {
    printf("usage: ping <ip|name> [-c count] [-t timeout_ms]\n");
}

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    const char* target_str = 0;
    uint32_t count = 4;
    uint32_t timeout_ms = 1000;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            uint32_t tmp = 0;
            if (!parse_uint32(argv[++i], &tmp) || tmp == 0) {
                print_usage();
                return 1;
            }
            count = tmp;
            continue;
        }
        if (argv[i][0] == '-' && argv[i][1] == 'c' && argv[i][2] != '\0') {
            uint32_t tmp = 0;
            if (!parse_uint32(argv[i] + 2, &tmp) || tmp == 0) {
                print_usage();
                return 1;
            }
            count = tmp;
            continue;
        }
        if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            uint32_t tmp = 0;
            if (!parse_uint32(argv[++i], &tmp) || tmp == 0) {
                print_usage();
                return 1;
            }
            timeout_ms = tmp;
            continue;
        }
        if (argv[i][0] == '-' && argv[i][1] == 't' && argv[i][2] != '\0') {
            uint32_t tmp = 0;
            if (!parse_uint32(argv[i] + 2, &tmp) || tmp == 0) {
                print_usage();
                return 1;
            }
            timeout_ms = tmp;
            continue;
        }
        if (!target_str) {
            target_str = argv[i];
            continue;
        }
        print_usage();
        return 1;
    }

    int fd_r = -1;
    int fd_w = -1;
    if (connect_networkd(&fd_r, &fd_w) != 0) {
        printf("ping: cannot connect to networkd\n");
        return 1;
    }

    net_ipc_rx_t rx;
    net_ipc_rx_reset(&rx);

    uint32_t seq = 1;
    (void)net_ipc_send(fd_w, NET_IPC_MSG_HELLO, seq++, 0, 0);

    uint32_t target_addr = 0;
    int target_is_ip = parse_ip4(target_str, &target_addr);
    if (!target_is_ip) {
        net_dns_req_t req;
        memset(&req, 0, sizeof(req));
        req.timeout_ms = timeout_ms;
        strncpy(req.name, target_str, (uint32_t)sizeof(req.name) - 1u);

        uint32_t dns_ipc_seq = seq++;
        (void)net_ipc_send(fd_w, NET_IPC_MSG_DNS_REQ, dns_ipc_seq, &req, (uint32_t)sizeof(req));

        net_dns_resp_t resp;
        if (dns_wait(fd_r, &rx, dns_ipc_seq, &resp, timeout_ms) != 0 || resp.status != NET_STATUS_OK || resp.addr == 0) {
            printf("ping: cannot resolve %s\n", target_str);
            close(fd_r);
            if (fd_w != fd_r) close(fd_w);
            return 1;
        }

        target_addr = resp.addr;
    }

    char ip_buf[32];
    ip4_to_str(target_addr, ip_buf, (uint32_t)sizeof(ip_buf));
    if (target_is_ip) {
        printf("PING %s (%s) 56(84) bytes of data.\n", ip_buf, ip_buf);
    } else {
        printf("PING %s (%s) 56(84) bytes of data.\n", target_str, ip_buf);
    }

    uint32_t transmitted = 0;
    uint32_t received = 0;
    uint32_t unreachable = 0;
    uint32_t timeouts = 0;
    uint32_t t_start = uptime_ms();

    for (uint32_t i = 0; i < count; i++) {
        net_ping_req_t req;
        req.addr = target_addr;
        req.timeout_ms = timeout_ms;
        req.seq = i + 1u;

        uint32_t send_seq = seq++;
        (void)net_ipc_send(fd_w, NET_IPC_MSG_PING_REQ, send_seq, &req, (uint32_t)sizeof(req));
        transmitted++;

        net_ping_resp_t resp;
        if (ping_wait(fd_r, &rx, req.seq, &resp, timeout_ms) == 0) {
            if (resp.status == NET_STATUS_OK) {
                printf("64 bytes from %s: icmp_seq=%u time=%ums\n", ip_buf, resp.seq, resp.rtt_ms);
                received++;
            } else if (resp.status == NET_STATUS_UNREACHABLE) {
                printf("From %s icmp_seq=%u Destination Host Unreachable\n", ip_buf, resp.seq);
                unreachable++;
            } else if (resp.status == NET_STATUS_TIMEOUT) {
                printf("Request timeout for icmp_seq=%u\n", resp.seq);
                timeouts++;
            } else {
                printf("From %s icmp_seq=%u Error\n", ip_buf, resp.seq);
            }
        } else {
            printf("Request timeout for icmp_seq=%u\n", req.seq);
            timeouts++;
        }
    }

    uint32_t t_end = uptime_ms();
    uint32_t loss = 0;
    if (transmitted > 0) {
        uint32_t lost = transmitted - received;
        loss = (lost * 100u) / transmitted;
    }
    printf("--- %s ping statistics ---\n", ip_buf);
    printf("%u packets transmitted, %u received, %u%% packet loss, time %ums\n", transmitted, received, loss, t_end - t_start);
    if (unreachable > 0 || timeouts > 0) {
        printf("%u unreachable, %u timeouts\n", unreachable, timeouts);
    }

    close(fd_r);
    if (fd_w != fd_r) close(fd_w);
    return 0;
}
