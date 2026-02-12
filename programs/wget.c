// SPDX-License-Identifier: GPL-2.0

#include <yula.h>

#include <net_ipc.h>

typedef struct {
    const char* url;
    const char* out_path;
    uint32_t timeout_ms;
    int quiet;
} wget_opts_t;

static void wget_print_usage(void) {
    printf("Usage: wget <url> [-O <file>|-] [--timeout <ms>] [-q]\n");
}

static int wget_parse_u32(const char* s, uint32_t* out) {
    if (!s || !*s || !out) {
        return 0;
    }

    uint32_t v = 0;
    for (const char* p = s; *p; p++) {
        if (*p < '0' || *p > '9') {
            return 0;
        }
        uint32_t d = (uint32_t)(*p - '0');
        if (v > (0xFFFFFFFFu - d) / 10u) {
            return 0;
        }
        v = v * 10u + d;
    }

    *out = v;
    return 1;
}

static void wget_fill_defaults(wget_opts_t* opts) {
    if (!opts) {
        return;
    }

    opts->url = 0;
    opts->out_path = 0;
    opts->timeout_ms = 15000u;
    opts->quiet = 0;
}

static int wget_parse_args(int argc, char** argv, wget_opts_t* out) {
    if (!out) {
        return -1;
    }

    wget_opts_t opts;
    wget_fill_defaults(&opts);

    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        if (!a) {
            continue;
        }

        if (strcmp(a, "-q") == 0) {
            opts.quiet = 1;
            continue;
        }

        if (strcmp(a, "-O") == 0) {
            if (i + 1 >= argc) {
                return -1;
            }
            opts.out_path = argv[i + 1];
            i++;
            continue;
        }

        if (strcmp(a, "--timeout") == 0) {
            if (i + 1 >= argc) {
                return -1;
            }

            uint32_t v = 0;
            if (!wget_parse_u32(argv[i + 1], &v)) {
                return -1;
            }
            opts.timeout_ms = v;
            i++;
            continue;
        }

        if (a[0] == '-') {
            return -1;
        }

        if (!opts.url) {
            opts.url = a;
            continue;
        }

        return -1;
    }

    if (!opts.url) {
        return -1;
    }

    *out = opts;
    return 0;
}

static const char* wget_find_path_start(const char* url) {
    if (!url) {
        return 0;
    }

    const char* s = url;

    const char* p = strstr(s, "://");
    if (p) {
        s = p + 3;
    }

    while (*s == '/') {
        s++;
    }

    const char* slash = strchr(s, '/');
    if (!slash) {
        return 0;
    }

    return slash;
}

static const char* wget_last_path_segment(const char* path) {
    if (!path) {
        return 0;
    }

    const char* last = path;
    for (const char* p = path; *p; p++) {
        if (*p == '/') {
            last = p + 1;
        }
    }

    return last;
}

static int wget_default_out_path(const char* url, char out_path[256]) {
    if (!url || !out_path) {
        return -1;
    }

    const char* path = wget_find_path_start(url);
    if (!path) {
        strcpy(out_path, "index.html");
        return 0;
    }

    const char* last = wget_last_path_segment(path);
    if (!last || !*last) {
        strcpy(out_path, "index.html");
        return 0;
    }

    uint32_t len = (uint32_t)strlen(last);
    if (len == 0 || len >= 255u) {
        strcpy(out_path, "index.html");
        return 0;
    }

    memcpy(out_path, last, len);
    out_path[len] = '\0';
    return 0;
}

static void wget_close_fds(int fd_r, int fd_w) {
    if (fd_r >= 0) {
        close(fd_r);
    }
    if (fd_w >= 0 && fd_w != fd_r) {
        close(fd_w);
    }
}

static uint32_t wget_timeout_with_slack(uint32_t timeout_ms) {
    if (timeout_ms == 0) {
        return 0;
    }

    uint32_t slack_ms = 500u;
    if (timeout_ms > (0xFFFFFFFFu - slack_ms)) {
        return 0xFFFFFFFFu;
    }

    return timeout_ms + slack_ms;
}

static int wget_write_all_timeout(int fd, const void* data, uint32_t len, uint32_t timeout_ms) {
    if (len == 0) {
        return 0;
    }
    if (!data) {
        return -1;
    }

    if (timeout_ms == 0) {
        timeout_ms = 5000u;
    }

    const uint8_t* p = (const uint8_t*)data;
    uint32_t off = 0;

    uint32_t start_ms = uptime_ms();

    while (off < len) {
        int wn = pipe_try_write(fd, p + off, len - off);
        if (wn < 0) {
            return -1;
        }
        if (wn > 0) {
            off += (uint32_t)wn;
            continue;
        }

        uint32_t now_ms = uptime_ms();
        uint32_t elapsed_ms = now_ms - start_ms;
        if (elapsed_ms >= timeout_ms) {
            return -1;
        }

        uint32_t remaining_ms = timeout_ms - elapsed_ms;
        int wait_ms = (remaining_ms < 50u) ? (int)remaining_ms : 50;
        if (wait_ms <= 0) {
            return -1;
        }

        pollfd_t pfd;
        pfd.fd = fd;
        pfd.events = POLLOUT;
        pfd.revents = 0;

        int pr = poll(&pfd, 1, wait_ms);
        if (pr < 0) {
            return -1;
        }
        if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            return -1;
        }
    }

    return 0;
}

static int wget_ipc_send_timeout(int fd, uint16_t type, uint32_t seq, const void* payload, uint32_t len, uint32_t timeout_ms) {
    if (len > NET_IPC_MAX_PAYLOAD) {
        return -1;
    }

    net_ipc_hdr_t hdr;
    hdr.magic = NET_IPC_MAGIC;
    hdr.version = NET_IPC_VERSION;
    hdr.type = type;
    hdr.len = len;
    hdr.seq = seq;

    if (wget_write_all_timeout(fd, &hdr, (uint32_t)sizeof(hdr), timeout_ms) != 0) {
        return -1;
    }

    if (len == 0) {
        return 0;
    }

    if (wget_write_all_timeout(fd, payload, len, timeout_ms) != 0) {
        return -1;
    }

    return 0;
}

static int wget_connect_networkd(int* out_r, int* out_w) {
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

static int wget_recv_one(int fd, net_ipc_rx_t* rx, net_ipc_hdr_t* out_hdr, uint8_t* out_payload, uint32_t cap, uint32_t timeout_ms) {
    if (!rx || !out_hdr) {
        return -1;
    }

    if (timeout_ms == 0) {
        timeout_ms = 5000u;
    }

    uint32_t start_ms = uptime_ms();
    uint32_t waited_ms = 0;
    for (;;) {
        int r = net_ipc_try_recv(rx, fd, out_hdr, out_payload, cap);
        if (r < 0) {
            return -1;
        }
        if (r > 0) {
            return 1;
        }

        uint32_t now_ms = uptime_ms();
        if ((now_ms - start_ms) >= timeout_ms) {
            return 0;
        }
        if (waited_ms >= timeout_ms) {
            return 0;
        }

        uint32_t remaining_by_uptime = timeout_ms - (now_ms - start_ms);
        uint32_t remaining_by_waited = timeout_ms - waited_ms;
        uint32_t remaining_ms = remaining_by_uptime;
        if (remaining_ms > remaining_by_waited) {
            remaining_ms = remaining_by_waited;
        }

        int wait_ms = (remaining_ms < 50u) ? (int)remaining_ms : 50;
        if (wait_ms <= 0) {
            return 0;
        }

        pollfd_t pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        int pr = poll(&pfd, 1, wait_ms);
        if (pr < 0) {
            return -1;
        }

        if (pr == 0) {
            waited_ms += (uint32_t)wait_ms;
            continue;
        }

        if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            return -1;
        }

        sleep(1);
        waited_ms += 1u;
    }
}

static int wget_write_all(int fd, const void* data, uint32_t len) {
    if (len == 0) {
        return 0;
    }
    if (!data) {
        return -1;
    }

    const uint8_t* p = (const uint8_t*)data;
    uint32_t off = 0;
    while (off < len) {
        int wn = write(fd, p + off, (int)(len - off));
        if (wn <= 0) {
            return -1;
        }
        off += (uint32_t)wn;
    }

    return 0;
}

static const char* wget_status_str(uint32_t st) {
    if (st == NET_STATUS_OK) {
        return "ok";
    }
    if (st == NET_STATUS_TIMEOUT) {
        return "timeout";
    }
    if (st == NET_STATUS_UNREACHABLE) {
        return "unreachable";
    }
    if (st == NET_STATUS_UNSUPPORTED) {
        return "unsupported";
    }
    if (st == NET_STATUS_ERROR) {
        return "error";
    }
    return "unknown";
}

static const char* wget_http_stage_str(uint32_t stage) {
    if (stage == NET_HTTP_GET_STAGE_PARSE_URL) {
        return "parse url";
    }
    if (stage == NET_HTTP_GET_STAGE_DNS) {
        return "dns";
    }
    if (stage == NET_HTTP_GET_STAGE_CONNECT) {
        return "connect";
    }
    if (stage == NET_HTTP_GET_STAGE_TLS_HANDSHAKE) {
        return "tls handshake";
    }
    if (stage == NET_HTTP_GET_STAGE_SEND_REQUEST) {
        return "send request";
    }
    if (stage == NET_HTTP_GET_STAGE_RECV_HEADERS) {
        return "recv headers";
    }
    if (stage == NET_HTTP_GET_STAGE_RECV_BODY) {
        return "recv body";
    }
    return "unknown stage";
}

static const char* wget_tls_step_str(uint32_t step) {
    if (step == NET_HTTP_TLS_STEP_BUILD_CLIENT_HELLO) {
        return "build client hello";
    }
    if (step == NET_HTTP_TLS_STEP_SEND_CLIENT_HELLO) {
        return "send client hello";
    }
    if (step == NET_HTTP_TLS_STEP_RECV_SERVER_HELLO) {
        return "recv server hello";
    }
    if (step == NET_HTTP_TLS_STEP_PARSE_SERVER_HELLO) {
        return "parse server hello";
    }
    if (step == NET_HTTP_TLS_STEP_RECV_SERVER_FINISHED) {
        return "recv server finished";
    }
    if (step == NET_HTTP_TLS_STEP_SEND_CLIENT_FINISHED) {
        return "send client finished";
    }
    if (step == NET_HTTP_TLS_STEP_DONE) {
        return "done";
    }
    return "unknown step";
}

static void wget_on_stage(const wget_opts_t* opts, const net_http_get_stage_t* st, uint32_t* last_stage) {
    if (!opts || !st || !last_stage) {
        return;
    }
    *last_stage = st->stage;
}

int main(int argc, char** argv) {
    wget_opts_t opts;
    if (wget_parse_args(argc, argv, &opts) != 0) {
        wget_print_usage();
        return 1;
    }

    char out_path_buf[256];
    const char* out_path = opts.out_path;
    if (!out_path) {
        if (wget_default_out_path(opts.url, out_path_buf) != 0) {
            printf("wget: cannot choose output file\n");
            return 1;
        }
        out_path = out_path_buf;
    }

    int out_fd = 1;
    int close_out = 0;
    if (strcmp(out_path, "-") != 0) {
        out_fd = open(out_path, 1);
        if (out_fd < 0) {
            printf("wget: cannot open %s\n", out_path);
            return 1;
        }
        close_out = 1;
    }

    int fd_r = -1;
    int fd_w = -1;
    if (wget_connect_networkd(&fd_r, &fd_w) != 0) {
        if (close_out) {
            close(out_fd);
        }
        printf("wget: cannot connect to networkd\n");
        return 1;
    }

    net_ipc_rx_t rx;
    net_ipc_rx_reset(&rx);

    uint32_t seq = 1;

    uint32_t hello_seq = seq;
    seq++;
    if (wget_ipc_send_timeout(fd_w, NET_IPC_MSG_HELLO, hello_seq, 0, 0, 1000u) != 0) {
        wget_close_fds(fd_r, fd_w);
        if (close_out) {
            close(out_fd);
        }
        printf("wget: ipc error\n");
        return 1;
    }

    {
        net_ipc_hdr_t hdr;
        uint8_t payload[NET_IPC_MAX_PAYLOAD];
        int r = wget_recv_one(fd_r, &rx, &hdr, payload, (uint32_t)sizeof(payload), 1000u);
        if (r <= 0 || hdr.type != NET_IPC_MSG_STATUS_RESP || hdr.seq != hello_seq) {
            wget_close_fds(fd_r, fd_w);
            if (close_out) {
                close(out_fd);
            }
            printf("wget: networkd not responding\n");
            return 1;
        }
    }

    net_http_get_req_t req;
    memset(&req, 0, sizeof(req));
    req.timeout_ms = opts.timeout_ms;
    req.flags = 0;
    strncpy(req.url, opts.url, (uint32_t)sizeof(req.url) - 1u);
    req.url[(uint32_t)sizeof(req.url) - 1u] = '\0';

    uint32_t get_seq = seq;
    seq++;

    if (wget_ipc_send_timeout(fd_w, NET_IPC_MSG_HTTP_GET_REQ, get_seq, &req, (uint32_t)sizeof(req), 1000u) != 0) {
        wget_close_fds(fd_r, fd_w);
        if (close_out) {
            close(out_fd);
        }
        printf("wget: ipc error\n");
        return 1;
    }

    net_http_get_begin_t begin;
    memset(&begin, 0, sizeof(begin));
    uint32_t last_stage = 0;

    uint32_t last_tls_detail = 0;
    uint32_t recv_timeout_ms = wget_timeout_with_slack(opts.timeout_ms);
    for (;;) {
        net_ipc_hdr_t hdr;
        uint8_t payload[NET_IPC_MAX_PAYLOAD];


        int r = wget_recv_one(fd_r, &rx, &hdr, payload, (uint32_t)sizeof(payload), recv_timeout_ms);
        if (r <= 0) {
            wget_close_fds(fd_r, fd_w);
            if (close_out) {
                close(out_fd);
            }
            if (last_stage != 0) {
                if (last_stage == NET_HTTP_GET_STAGE_TLS_HANDSHAKE && last_tls_detail != 0) {
                    uint32_t step = NET_HTTP_TLS_DETAIL_STEP(last_tls_detail);
                    printf("wget: timeout at %s (%s)\n", wget_http_stage_str(last_stage), wget_tls_step_str(step));
                } else {
                    printf("wget: timeout at %s\n", wget_http_stage_str(last_stage));
                }
            } else {
                printf("wget: timeout waiting begin\n");
            }
            return 1;
        }

        if (hdr.seq != get_seq) {
            continue;
        }

        if (hdr.type == NET_IPC_MSG_HTTP_GET_STAGE) {
            if (hdr.len == (uint32_t)sizeof(net_http_get_stage_t)) {
                net_http_get_stage_t st;
                memcpy(&st, payload, sizeof(st));
                wget_on_stage(&opts, &st, &last_stage);
                if (st.stage == NET_HTTP_GET_STAGE_TLS_HANDSHAKE && (st.flags & NET_HTTP_GET_STAGE_F_END) != 0u) {
                    last_tls_detail = st.detail;
                }
            }
            continue;
        }

        if (hdr.type != NET_IPC_MSG_HTTP_GET_BEGIN) {
            continue;
        }

        if (hdr.len != (uint32_t)sizeof(begin)) {
            wget_close_fds(fd_r, fd_w);
            if (close_out) {
                close(out_fd);
            }
            printf("wget: invalid response\n");
            return 1;
        }

        memcpy(&begin, payload, sizeof(begin));
        break;
    }

    if (begin.status != NET_STATUS_OK) {
        wget_close_fds(fd_r, fd_w);
        if (close_out) {
            close(out_fd);
        }
        if (begin.status == NET_STATUS_TIMEOUT && last_stage != 0) {
            if (last_stage == NET_HTTP_GET_STAGE_TLS_HANDSHAKE && last_tls_detail != 0) {
                uint32_t step = NET_HTTP_TLS_DETAIL_STEP(last_tls_detail);
                printf("wget: timeout at %s (%s)\n", wget_http_stage_str(last_stage), wget_tls_step_str(step));
            } else {
                printf("wget: timeout at %s\n", wget_http_stage_str(last_stage));
            }
        } else {
            printf("wget: %s\n", wget_status_str(begin.status));
        }
        return 1;
    }

    if (!opts.quiet) {
        if (begin.content_length > 0) {
            printf("wget: HTTP %u, %u bytes\n", begin.http_status, begin.content_length);
        } else {
            printf("wget: HTTP %u\n", begin.http_status);
        }
    }

    uint32_t total = 0;
    uint32_t final_status = NET_STATUS_ERROR;

    for (;;) {
        net_ipc_hdr_t hdr;
        uint8_t payload[NET_IPC_MAX_PAYLOAD];

        int r = wget_recv_one(fd_r, &rx, &hdr, payload, (uint32_t)sizeof(payload), recv_timeout_ms);
        if (r <= 0) {
            final_status = NET_STATUS_TIMEOUT;
            break;
        }

        if (hdr.seq != get_seq) {
            continue;
        }

        if (hdr.type == NET_IPC_MSG_HTTP_GET_STAGE) {
            if (hdr.len == (uint32_t)sizeof(net_http_get_stage_t)) {
                net_http_get_stage_t st;
                memcpy(&st, payload, sizeof(st));
                wget_on_stage(&opts, &st, &last_stage);
                if (st.stage == NET_HTTP_GET_STAGE_TLS_HANDSHAKE && (st.flags & NET_HTTP_GET_STAGE_F_END) != 0u) {
                    last_tls_detail = st.detail;
                }
            }
            continue;
        }

        if (hdr.type == NET_IPC_MSG_HTTP_GET_DATA) {
            if (hdr.len > 0) {
                if (wget_write_all(out_fd, payload, hdr.len) != 0) {
                    final_status = NET_STATUS_ERROR;
                    break;
                }
                total += hdr.len;
            }
            continue;
        }

        if (hdr.type == NET_IPC_MSG_HTTP_GET_END) {
            net_http_get_end_t end;
            if (hdr.len != (uint32_t)sizeof(end)) {
                final_status = NET_STATUS_ERROR;
                break;
            }
            memcpy(&end, payload, sizeof(end));
            final_status = end.status;
            break;
        }
    }

    wget_close_fds(fd_r, fd_w);
    if (close_out) {
        close(out_fd);
    }

    if (final_status != NET_STATUS_OK) {
        if (final_status == NET_STATUS_TIMEOUT && last_stage != 0) {
            if (last_stage == NET_HTTP_GET_STAGE_TLS_HANDSHAKE && last_tls_detail != 0) {
                uint32_t step = NET_HTTP_TLS_DETAIL_STEP(last_tls_detail);
                printf("wget: timeout at %s (%s)\n", wget_http_stage_str(last_stage), wget_tls_step_str(step));
            } else {
                printf("wget: timeout at %s\n", wget_http_stage_str(last_stage));
            }
        } else {
            printf("wget: %s\n", wget_status_str(final_status));
        }
        return 1;
    }

    if (!opts.quiet && strcmp(out_path, "-") != 0) {
        printf("wget: saved %u bytes to %s\n", total, out_path);
    }

    return 0;
}
