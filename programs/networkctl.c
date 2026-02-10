// SPDX-License-Identifier: GPL-2.0

#include <yula.h>
#include <net_ipc.h>

static void ip4_to_str(uint32_t addr, char* out, uint32_t cap) {
    if (!out || cap == 0) {
        return;
    }
    uint8_t a = (uint8_t)((addr >> 24) & 0xFFu);
    uint8_t b = (uint8_t)((addr >> 16) & 0xFFu);
    uint8_t c = (uint8_t)((addr >> 8) & 0xFFu);
    uint8_t d = (uint8_t)(addr & 0xFFu);
    snprintf(out, cap, "%u.%u.%u.%u", a, b, c, d);
}

static void mac_to_str(const uint8_t mac[6], char* out, uint32_t cap) {
    if (!out || cap == 0 || !mac) {
        return;
    }

    snprintf(
        out,
        cap,
        "%02x:%02x:%02x:%02x:%02x:%02x",
        mac[0],
        mac[1],
        mac[2],
        mac[3],
        mac[4],
        mac[5]
    );
}

static int netctl_parse_u32(const char* s, uint32_t* out) {
    if (!s || !*s || !out) {
        return 0;
    }

    uint32_t v = 0;
    for (const char* p = s; *p; p++) {
        if (*p < '0' || *p > '9') {
            return 0;
        }

        uint32_t digit = (uint32_t)(*p - '0');
        if (v > (0xFFFFFFFFu - digit) / 10u) {
            return 0;
        }

        v = (v * 10u) + digit;
    }

    *out = v;
    return 1;
}

static int netctl_parse_ip4(const char* s, uint32_t* out) {
    if (!s || !*s || !out) {
        return 0;
    }

    uint32_t parts[4];
    for (int i = 0; i < 4; i++) {
        parts[i] = 0;
    }

    const char* p = s;
    for (int i = 0; i < 4; i++) {
        if (*p == '\0') {
            return 0;
        }

        uint32_t v = 0;
        uint32_t digits = 0;

        while (*p >= '0' && *p <= '9') {
            digits++;
            if (digits > 3) {
                return 0;
            }

            v = (v * 10u) + (uint32_t)(*p - '0');
            if (v > 255u) {
                return 0;
            }

            p++;
        }

        if (digits == 0) {
            return 0;
        }

        parts[i] = v;

        if (i != 3) {
            if (*p != '.') {
                return 0;
            }
            p++;
        }
    }

    if (*p != '\0') {
        return 0;
    }

    *out =
        (parts[0] << 24) |
        (parts[1] << 16) |
        (parts[2] << 8) |
        parts[3];
    return 1;
}

static int netctl_wait(int fd, net_ipc_rx_t* rx, uint16_t want_type, uint32_t want_seq, net_ipc_hdr_t* out_hdr, uint8_t* out_payload, uint32_t cap, uint32_t timeout_ms) {
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
        if (now - start >= timeout_ms) {
            return -1;
        }

        pollfd_t pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        (void)poll(&pfd, 1, 50);
    }
}

static int netctl_connect(int* out_r, int* out_w) {
    int fds[2];
    if (ipc_connect("networkd", fds) != 0) {
        return -1;
    }
    *out_r = fds[0];
    *out_w = fds[1];
    return 0;
}

static void netctl_close(int fd_r, int fd_w) {
    if (fd_r >= 0) {
        close(fd_r);
    }
    if (fd_w >= 0 && fd_w != fd_r) {
        close(fd_w);
    }
}

static void netctl_print_links(const uint8_t* payload, uint32_t len) {
    if (!payload || len < sizeof(net_link_list_hdr_t)) {
        printf("links: invalid response\n");
        return;
    }

    net_link_list_hdr_t hdr;
    memcpy(&hdr, payload, sizeof(hdr));

    uint32_t expected = (uint32_t)sizeof(net_link_list_hdr_t) + hdr.count * (uint32_t)sizeof(net_link_info_t);
    if (len < expected) {
        printf("links: truncated response\n");
        return;
    }

    const uint8_t* ptr = payload + sizeof(net_link_list_hdr_t);
    for (uint32_t i = 0; i < hdr.count; i++) {
        net_link_info_t info;
        memcpy(&info, ptr, sizeof(info));
        ptr += sizeof(info);

        char ip_str[32];
        char mask_str[32];
        char mac_str[32];
        ip4_to_str(info.ipv4_addr, ip_str, (uint32_t)sizeof(ip_str));
        ip4_to_str(info.ipv4_mask, mask_str, (uint32_t)sizeof(mask_str));
        mac_to_str(info.mac, mac_str, (uint32_t)sizeof(mac_str));

        const char* state = (info.flags & NET_LINK_FLAG_UP) ? "up" : "down";
        const char* kind = (info.flags & NET_LINK_FLAG_LOOPBACK) ? "loopback" : "ethernet";
        printf("%s  %s  %s  %s/%s  %s\n", info.name, kind, state, ip_str, mask_str, mac_str);
    }
}

static void netctl_print_cfg(const net_cfg_resp_t* cfg) {
    if (!cfg) {
        return;
    }

    char ip[32];
    char mask[32];
    char gw[32];
    char dns[32];

    ip4_to_str(cfg->ip, ip, (uint32_t)sizeof(ip));
    ip4_to_str(cfg->mask, mask, (uint32_t)sizeof(mask));
    ip4_to_str(cfg->gw, gw, (uint32_t)sizeof(gw));
    ip4_to_str(cfg->dns, dns, (uint32_t)sizeof(dns));

    printf("config:\n");
    printf("  ip:   %s\n", ip);
    printf("  mask: %s\n", mask);
    printf("  gw:   %s\n", gw);
    printf("  dns:  %s\n", dns);
}

static void netctl_print_usage(void) {
    printf("networkctl - network manager control tool\n\n");
    printf("usage:\n");
    printf("  networkctl\n");
    printf("  networkctl status\n");
    printf("  networkctl links\n");
    printf("  networkctl ping <ip|name> [-c count] [-t timeout_ms]\n");
    printf("  networkctl resolve <name> [-t timeout_ms]\n");
    printf("  networkctl config show\n");
    printf("  networkctl config set [ip A.B.C.D] [mask A.B.C.D] [gw A.B.C.D] [dns A.B.C.D]\n");
    printf("  networkctl up\n");
    printf("  networkctl down\n");
    printf("  networkctl daemon status\n");
    printf("  networkctl daemon start\n");
    printf("  networkctl daemon stop\n");
    printf("  networkctl daemon restart\n");
}

static const char* netctl_proc_state_name(uint32_t st) {
    switch (st) {
        case 0: return "UNUSED";
        case 1: return "RUNNABLE";
        case 2: return "RUNNING";
        case 3: return "ZOMBIE";
        case 4: return "WAITING";
        default: return "?";
    }
}

static const char* netctl_basename(const char* path) {
    if (!path) {
        return 0;
    }

    const char* base = path;
    for (const char* p = path; *p; p++) {
        if (*p == '/') {
            base = p + 1;
        }
    }

    return base;
}

static int netctl_name_equals_strip_exe(const char* name, const char* want) {
    if (!name || !want) {
        return 0;
    }

    size_t name_len = strlen(name);
    if (name_len >= 4u && strcmp(name + (name_len - 4u), ".exe") == 0) {
        name_len -= 4u;
    }

    size_t want_len = strlen(want);
    if (want_len != name_len) {
        return 0;
    }

    return memcmp(name, want, want_len) == 0;
}

static int netctl_proc_name_matches(const char* proc_name, const char* want_name) {
    if (!proc_name || !want_name) {
        return 0;
    }

    if (netctl_name_equals_strip_exe(proc_name, want_name)) {
        return 1;
    }

    const char* base = netctl_basename(proc_name);
    if (base && netctl_name_equals_strip_exe(base, want_name)) {
        return 1;
    }

    return 0;
}

static int netctl_find_process(const char* name, yos_proc_info_t* out) {
    if (!name || !*name || !out) {
        return 0;
    }

    uint32_t cap = 32;
    yos_proc_info_t* list = 0;

    for (;;) {
        if (cap == 0 || cap > (0xFFFFFFFFu / (uint32_t)sizeof(*list))) {
            if (list) {
                free(list);
            }
            return 0;
        }

        uint32_t bytes = cap * (uint32_t)sizeof(*list);
        yos_proc_info_t* next = (yos_proc_info_t*)realloc(list, bytes);
        if (!next) {
            if (list) {
                free(list);
            }
            return 0;
        }
        list = next;

        int n = proc_list(list, cap);
        if (n < 0) {
            free(list);
            return 0;
        }

        if ((uint32_t)n == cap) {
            uint32_t next_cap = cap * 2u;
            if (next_cap <= cap) {
                free(list);
                return 0;
            }
            cap = next_cap;
            continue;
        }

        for (int i = 0; i < n; i++) {
            if (netctl_proc_name_matches(list[i].name, name)) {
                *out = list[i];
                free(list);
                return 1;
            }
        }

        free(list);
        return 0;
    }
}

static int netctl_cmd_daemon(int argc, char** argv) {
    const char* sub = "status";
    if (argc >= 1) {
        sub = argv[0];
    }
    yos_proc_info_t info;
    int running = netctl_find_process("networkd", &info);

    if (strcmp(sub, "status") == 0) {
        if (!running) {
            printf("daemon: stopped\n");
            return 0;
        }
        printf("daemon: running\n");
        printf("pid: %u\n", info.pid);
        printf("state: %s\n", netctl_proc_state_name(info.state));
        return 0;
    }

    if (strcmp(sub, "stop") == 0) {
        if (!running) {
            printf("daemon: already stopped\n");
            return 0;
        }

        if (kill((int)info.pid) != 0) {
            printf("daemon: kill failed\n");
            return 1;
        }

        printf("daemon: stopped\n");
        return 0;
    }

    if (strcmp(sub, "start") == 0) {
        if (running) {
            printf("daemon: already running (pid %u)\n", info.pid);
            return 0;
        }

        char* args[2];
        args[0] = (char*)"networkd";
        args[1] = 0;

        int pid = spawn_process_resolved("networkd", 1, args);
        if (pid < 0) {
            printf("daemon: spawn failed\n");
            return 1;
        }

        printf("daemon: started (pid %d)\n", pid);
        return 0;
    }

    if (strcmp(sub, "restart") == 0) {
        if (running) {
            (void)kill((int)info.pid);
            sleep(50);
        }

        char* args[2];
        args[0] = (char*)"networkd";
        args[1] = 0;

        int pid = spawn_process_resolved("networkd", 1, args);
        if (pid < 0) {
            printf("daemon: spawn failed\n");
            return 1;
        }

        printf("daemon: restarted (pid %d)\n", pid);
        return 0;
    }

    netctl_print_usage();
    return 1;
}

static int netctl_send_hello(int fd_w, uint32_t* io_seq) {
    if (!io_seq) {
        return -1;
    }

    uint32_t seq = *io_seq;
    (void)net_ipc_send(fd_w, NET_IPC_MSG_HELLO, seq, 0, 0);
    *io_seq = seq + 1u;
    return 0;
}

static int netctl_dns_query(int fd_r, int fd_w, net_ipc_rx_t* rx, uint32_t* io_seq, const char* name, uint32_t timeout_ms, uint32_t* out_addr) {
    if (!rx || !io_seq || !name || !*name || !out_addr) {
        return -1;
    }

    net_dns_req_t req;
    memset(&req, 0, sizeof(req));
    req.timeout_ms = timeout_ms;
    strncpy(req.name, name, (uint32_t)sizeof(req.name) - 1u);

    uint32_t seq = *io_seq;
    *io_seq = seq + 1u;

    if (net_ipc_send(fd_w, NET_IPC_MSG_DNS_REQ, seq, &req, (uint32_t)sizeof(req)) != 0) {
        return -1;
    }

    net_ipc_hdr_t hdr;
    net_dns_resp_t resp;
    if (netctl_wait(fd_r, rx, NET_IPC_MSG_DNS_RESP, seq, &hdr, (uint8_t*)&resp, (uint32_t)sizeof(resp), timeout_ms) != 0) {
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

static int netctl_cmd_ping(int argc, char** argv) {
    if (argc < 1) {
        netctl_print_usage();
        return 1;
    }

    const char* target_str = argv[0];

    uint32_t count = 4;
    uint32_t timeout_ms = 1000;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            uint32_t tmp = 0;
            if (!netctl_parse_u32(argv[++i], &tmp) || tmp == 0) {
                netctl_print_usage();
                return 1;
            }
            count = tmp;
            continue;
        }

        if (argv[i][0] == '-' && argv[i][1] == 'c' && argv[i][2] != '\0') {
            uint32_t tmp = 0;
            if (!netctl_parse_u32(argv[i] + 2, &tmp) || tmp == 0) {
                netctl_print_usage();
                return 1;
            }
            count = tmp;
            continue;
        }

        if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            uint32_t tmp = 0;
            if (!netctl_parse_u32(argv[++i], &tmp) || tmp == 0) {
                netctl_print_usage();
                return 1;
            }
            timeout_ms = tmp;
            continue;
        }

        if (argv[i][0] == '-' && argv[i][1] == 't' && argv[i][2] != '\0') {
            uint32_t tmp = 0;
            if (!netctl_parse_u32(argv[i] + 2, &tmp) || tmp == 0) {
                netctl_print_usage();
                return 1;
            }
            timeout_ms = tmp;
            continue;
        }

        netctl_print_usage();
        return 1;
    }

    int fd_r = -1;
    int fd_w = -1;
    if (netctl_connect(&fd_r, &fd_w) != 0) {
        printf("networkctl: cannot connect to networkd\n");
        return 1;
    }

    net_ipc_rx_t rx;
    net_ipc_rx_reset(&rx);

    uint32_t seq = 1;
    (void)netctl_send_hello(fd_w, &seq);

    uint32_t dst_ip = 0;
    int is_ip = netctl_parse_ip4(target_str, &dst_ip);

    if (!is_ip) {
        if (netctl_dns_query(fd_r, fd_w, &rx, &seq, target_str, timeout_ms, &dst_ip) != 0) {
            printf("ping: cannot resolve %s\n", target_str);
            netctl_close(fd_r, fd_w);
            return 1;
        }
    }

    char ip_buf[32];
    ip4_to_str(dst_ip, ip_buf, (uint32_t)sizeof(ip_buf));

    if (is_ip) {
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
        req.addr = dst_ip;
        req.timeout_ms = timeout_ms;
        req.seq = i + 1u;

        uint32_t msg_seq = seq;
        seq++;

        (void)net_ipc_send(fd_w, NET_IPC_MSG_PING_REQ, msg_seq, &req, (uint32_t)sizeof(req));
        transmitted++;

        net_ipc_hdr_t hdr;
        net_ping_resp_t resp;
        int ok = netctl_wait(fd_r, &rx, NET_IPC_MSG_PING_RESP, msg_seq, &hdr, (uint8_t*)&resp, (uint32_t)sizeof(resp), timeout_ms);
        if (ok != 0 || hdr.len != (uint32_t)sizeof(resp)) {
            printf("Request timeout for icmp_seq=%u\n", req.seq);
            timeouts++;
            continue;
        }

        if (resp.status == NET_STATUS_OK) {
            printf("64 bytes from %s: icmp_seq=%u time=%ums\n", ip_buf, resp.seq, resp.rtt_ms);
            received++;
            continue;
        }

        if (resp.status == NET_STATUS_UNREACHABLE) {
            printf("From %s icmp_seq=%u Destination Host Unreachable\n", ip_buf, resp.seq);
            unreachable++;
            continue;
        }

        if (resp.status == NET_STATUS_TIMEOUT) {
            printf("Request timeout for icmp_seq=%u\n", resp.seq);
            timeouts++;
            continue;
        }

        printf("From %s icmp_seq=%u Error\n", ip_buf, resp.seq);
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

    netctl_close(fd_r, fd_w);
    return (received > 0) ? 0 : 1;
}

static int netctl_cmd_resolve(int argc, char** argv) {
    if (argc < 1) {
        netctl_print_usage();
        return 1;
    }

    const char* name = argv[0];
    uint32_t timeout_ms = 1000;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            uint32_t tmp = 0;
            if (!netctl_parse_u32(argv[++i], &tmp) || tmp == 0) {
                netctl_print_usage();
                return 1;
            }
            timeout_ms = tmp;
            continue;
        }

        if (argv[i][0] == '-' && argv[i][1] == 't' && argv[i][2] != '\0') {
            uint32_t tmp = 0;
            if (!netctl_parse_u32(argv[i] + 2, &tmp) || tmp == 0) {
                netctl_print_usage();
                return 1;
            }
            timeout_ms = tmp;
            continue;
        }

        netctl_print_usage();
        return 1;
    }

    int fd_r = -1;
    int fd_w = -1;
    if (netctl_connect(&fd_r, &fd_w) != 0) {
        printf("networkctl: cannot connect to networkd\n");
        return 1;
    }

    net_ipc_rx_t rx;
    net_ipc_rx_reset(&rx);

    uint32_t seq = 1;
    (void)netctl_send_hello(fd_w, &seq);

    uint32_t addr = 0;
    if (netctl_dns_query(fd_r, fd_w, &rx, &seq, name, timeout_ms, &addr) != 0) {
        printf("resolve: failed\n");
        netctl_close(fd_r, fd_w);
        return 1;
    }

    char ip[32];
    ip4_to_str(addr, ip, (uint32_t)sizeof(ip));
    printf("%s -> %s\n", name, ip);

    netctl_close(fd_r, fd_w);
    return 0;
}

static int netctl_cmd_config(int argc, char** argv) {
    if (argc < 1) {
        netctl_print_usage();
        return 1;
    }

    const char* sub = argv[0];

    int fd_r = -1;
    int fd_w = -1;
    if (netctl_connect(&fd_r, &fd_w) != 0) {
        printf("networkctl: cannot connect to networkd\n");
        return 1;
    }

    net_ipc_rx_t rx;
    net_ipc_rx_reset(&rx);

    uint32_t seq = 1;
    (void)netctl_send_hello(fd_w, &seq);

    if (strcmp(sub, "show") == 0) {
        uint32_t msg_seq = seq;
        seq++;
        (void)net_ipc_send(fd_w, NET_IPC_MSG_CFG_GET_REQ, msg_seq, 0, 0);

        net_ipc_hdr_t hdr;
        net_cfg_resp_t resp;
        if (netctl_wait(fd_r, &rx, NET_IPC_MSG_CFG_GET_RESP, msg_seq, &hdr, (uint8_t*)&resp, (uint32_t)sizeof(resp), 1000) != 0) {
            printf("config: not available\n");
            netctl_close(fd_r, fd_w);
            return 1;
        }

        if (hdr.len != (uint32_t)sizeof(resp)) {
            printf("config: invalid response\n");
            netctl_close(fd_r, fd_w);
            return 1;
        }

        if (resp.status != NET_STATUS_OK) {
            printf("config: error\n");
            netctl_close(fd_r, fd_w);
            return 1;
        }

        netctl_print_cfg(&resp);
        netctl_close(fd_r, fd_w);
        return 0;
    }

    if (strcmp(sub, "set") == 0) {
        net_cfg_set_t req;
        memset(&req, 0, sizeof(req));

        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "ip") == 0 && i + 1 < argc) {
                uint32_t v = 0;
                if (!netctl_parse_ip4(argv[++i], &v)) {
                    netctl_print_usage();
                    netctl_close(fd_r, fd_w);
                    return 1;
                }
                req.flags |= NET_CFG_F_IP;
                req.ip = v;
                continue;
            }

            if (strcmp(argv[i], "mask") == 0 && i + 1 < argc) {
                uint32_t v = 0;
                if (!netctl_parse_ip4(argv[++i], &v)) {
                    netctl_print_usage();
                    netctl_close(fd_r, fd_w);
                    return 1;
                }
                req.flags |= NET_CFG_F_MASK;
                req.mask = v;
                continue;
            }

            if (strcmp(argv[i], "gw") == 0 && i + 1 < argc) {
                uint32_t v = 0;
                if (!netctl_parse_ip4(argv[++i], &v)) {
                    netctl_print_usage();
                    netctl_close(fd_r, fd_w);
                    return 1;
                }
                req.flags |= NET_CFG_F_GW;
                req.gw = v;
                continue;
            }

            if (strcmp(argv[i], "dns") == 0 && i + 1 < argc) {
                uint32_t v = 0;
                if (!netctl_parse_ip4(argv[++i], &v)) {
                    netctl_print_usage();
                    netctl_close(fd_r, fd_w);
                    return 1;
                }
                req.flags |= NET_CFG_F_DNS;
                req.dns = v;
                continue;
            }

            netctl_print_usage();
            netctl_close(fd_r, fd_w);
            return 1;
        }

        if (req.flags == 0u) {
            netctl_print_usage();
            netctl_close(fd_r, fd_w);
            return 1;
        }

        uint32_t msg_seq = seq;
        seq++;
        (void)net_ipc_send(fd_w, NET_IPC_MSG_CFG_SET_REQ, msg_seq, &req, (uint32_t)sizeof(req));

        net_ipc_hdr_t hdr;
        net_cfg_resp_t resp;
        if (netctl_wait(fd_r, &rx, NET_IPC_MSG_CFG_SET_RESP, msg_seq, &hdr, (uint8_t*)&resp, (uint32_t)sizeof(resp), 1000) != 0) {
            printf("config: set failed\n");
            netctl_close(fd_r, fd_w);
            return 1;
        }

        if (hdr.len != (uint32_t)sizeof(resp)) {
            printf("config: invalid response\n");
            netctl_close(fd_r, fd_w);
            return 1;
        }

        if (resp.status != NET_STATUS_OK) {
            printf("config: set error\n");
            netctl_close(fd_r, fd_w);
            return 1;
        }

        netctl_print_cfg(&resp);
        netctl_close(fd_r, fd_w);
        return 0;
    }

    netctl_print_usage();
    netctl_close(fd_r, fd_w);
    return 1;
}

static int netctl_cmd_iface(int up) {
    int fd_r = -1;
    int fd_w = -1;
    if (netctl_connect(&fd_r, &fd_w) != 0) {
        printf("networkctl: cannot connect to networkd\n");
        return 1;
    }

    net_ipc_rx_t rx;
    net_ipc_rx_reset(&rx);

    uint32_t seq = 1;
    (void)netctl_send_hello(fd_w, &seq);

    uint32_t req_type = up ? NET_IPC_MSG_IFACE_UP_REQ : NET_IPC_MSG_IFACE_DOWN_REQ;
    uint32_t resp_type = up ? NET_IPC_MSG_IFACE_UP_RESP : NET_IPC_MSG_IFACE_DOWN_RESP;

    uint32_t msg_seq = seq;
    seq++;
    (void)net_ipc_send(fd_w, (uint16_t)req_type, msg_seq, 0, 0);

    net_ipc_hdr_t hdr;
    net_status_resp_t resp;
    if (netctl_wait(fd_r, &rx, (uint16_t)resp_type, msg_seq, &hdr, (uint8_t*)&resp, (uint32_t)sizeof(resp), 1000) != 0) {
        printf("iface: no response\n");
        netctl_close(fd_r, fd_w);
        return 1;
    }

    if (hdr.len != (uint32_t)sizeof(resp)) {
        printf("iface: invalid response\n");
        netctl_close(fd_r, fd_w);
        return 1;
    }

    if (resp.status != NET_STATUS_OK) {
        printf("iface: error\n");
        netctl_close(fd_r, fd_w);
        return 1;
    }

    printf("iface: %s\n", up ? "up" : "down");
    netctl_close(fd_r, fd_w);
    return 0;
}

static int netctl_cmd_status(int show_links) {
    int fd_r = -1;
    int fd_w = -1;
    if (netctl_connect(&fd_r, &fd_w) != 0) {
        printf("networkctl: cannot connect to networkd\n");
        return 1;
    }

    net_ipc_rx_t rx;
    net_ipc_rx_reset(&rx);

    uint32_t seq = 1;
    (void)netctl_send_hello(fd_w, &seq);

    uint32_t status_seq = seq;
    seq++;
    (void)net_ipc_send(fd_w, NET_IPC_MSG_STATUS_REQ, status_seq, 0, 0);

    net_ipc_hdr_t hdr;
    net_status_resp_t st;
    if (netctl_wait(fd_r, &rx, NET_IPC_MSG_STATUS_RESP, status_seq, &hdr, (uint8_t*)&st, (uint32_t)sizeof(st), 1000) == 0) {
        if (hdr.len == (uint32_t)sizeof(st)) {
            const char* state = (st.status == NET_STATUS_OK) ? "running" : "error";
            printf("state: %s\n", state);
            printf("links: %u\n", st.link_count);
        } else {
            printf("state: invalid response\n");
        }
    } else {
        printf("state: unknown\n");
    }

    {
        uint32_t cfg_seq = seq;
        seq++;

        (void)net_ipc_send(fd_w, NET_IPC_MSG_CFG_GET_REQ, cfg_seq, 0, 0);

        net_cfg_resp_t cfg;
        if (netctl_wait(fd_r, &rx, NET_IPC_MSG_CFG_GET_RESP, cfg_seq, &hdr, (uint8_t*)&cfg, (uint32_t)sizeof(cfg), 1000) == 0) {
            if (hdr.len == (uint32_t)sizeof(cfg) && cfg.status == NET_STATUS_OK) {
                netctl_print_cfg(&cfg);
            }
        }
    }

    if (show_links) {
        uint32_t link_seq = seq;
        seq++;

        (void)net_ipc_send(fd_w, NET_IPC_MSG_LINK_LIST_REQ, link_seq, 0, 0);

        uint8_t payload[NET_IPC_MAX_PAYLOAD];
        if (netctl_wait(fd_r, &rx, NET_IPC_MSG_LINK_LIST_RESP, link_seq, &hdr, payload, (uint32_t)sizeof(payload), 1000) == 0) {
            netctl_print_links(payload, hdr.len);
        } else {
            printf("links: not available\n");
        }
    }

    netctl_close(fd_r, fd_w);
    return 0;
}

static int netctl_cmd_links(void) {
    int fd_r = -1;
    int fd_w = -1;
    if (netctl_connect(&fd_r, &fd_w) != 0) {
        printf("networkctl: cannot connect to networkd\n");
        return 1;
    }

    net_ipc_rx_t rx;
    net_ipc_rx_reset(&rx);

    uint32_t seq = 1;
    (void)netctl_send_hello(fd_w, &seq);

    uint32_t link_seq = seq;
    seq++;

    (void)net_ipc_send(fd_w, NET_IPC_MSG_LINK_LIST_REQ, link_seq, 0, 0);

    net_ipc_hdr_t hdr;
    uint8_t payload[NET_IPC_MAX_PAYLOAD];
    if (netctl_wait(fd_r, &rx, NET_IPC_MSG_LINK_LIST_RESP, link_seq, &hdr, payload, (uint32_t)sizeof(payload), 1000) != 0) {
        printf("links: not available\n");
        netctl_close(fd_r, fd_w);
        return 1;
    }

    netctl_print_links(payload, hdr.len);
    netctl_close(fd_r, fd_w);
    return 0;
}

int main(int argc, char** argv) {
    if (argc <= 1) {
        return netctl_cmd_status(1);
    }

    const char* cmd = argv[1];

    if (strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0 || strcmp(cmd, "help") == 0) {
        netctl_print_usage();
        return 0;
    }

    if (strcmp(cmd, "status") == 0) {
        return netctl_cmd_status(0);
    }

    if (strcmp(cmd, "links") == 0) {
        return netctl_cmd_links();
    }

    if (strcmp(cmd, "ping") == 0) {
        return netctl_cmd_ping(argc - 2, &argv[2]);
    }

    if (strcmp(cmd, "resolve") == 0) {
        return netctl_cmd_resolve(argc - 2, &argv[2]);
    }

    if (strcmp(cmd, "config") == 0) {
        return netctl_cmd_config(argc - 2, &argv[2]);
    }

    if (strcmp(cmd, "up") == 0) {
        return netctl_cmd_iface(1);
    }

    if (strcmp(cmd, "down") == 0) {
        return netctl_cmd_iface(0);
    }

    if (strcmp(cmd, "daemon") == 0) {
        return netctl_cmd_daemon(argc - 2, &argv[2]);
    }

    netctl_print_usage();
    return 1;
}
