#include <yula.h>

#include <yos/netd_ipc.h>

extern "C" unsigned long long __udivdi3(unsigned long long n, unsigned long long d) {
    if (d == 0ull) {
        return 0ull;
    }

    if (d == 1ull) {
        return n;
    }

    if (n < d) {
        return 0ull;
    }

    unsigned long long q = 0ull;
    unsigned long long r = 0ull;

    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1ull);
        if (r >= d) {
            r -= d;
            q |= (1ull << i);
        }
    }

    return q;
}

namespace ping {

static constexpr uint32_t kPayloadBytes = 56u;
static constexpr uint32_t kReplyLineBytes = 64u;
static constexpr uint32_t kDefaultTimeoutMs = 2000u;
static constexpr uint32_t kDefaultCount = 4u;
static constexpr uint32_t kDefaultIntervalMs = 100u;
static constexpr uint32_t kTtl = 64u;

static uint16_t bswap16(uint16_t v) {
    return (uint16_t)((v << 8) | (v >> 8));
}

static uint32_t bswap32(uint32_t v) {
    return ((v & 0x000000FFu) << 24) |
           ((v & 0x0000FF00u) << 8) |
           ((v & 0x00FF0000u) >> 8) |
           ((v & 0xFF000000u) >> 24);
}

static uint16_t htons(uint16_t v) { return bswap16(v); }
static uint32_t htonl(uint32_t v) { return bswap32(v); }
static uint32_t ntohl(uint32_t v) { return bswap32(v); }

static void ip_to_string(uint32_t ip_be, char out[16]) {
    const uint32_t ip = ntohl(ip_be);

    const uint32_t a = (ip >> 24) & 0xFFu;
    const uint32_t b = (ip >> 16) & 0xFFu;
    const uint32_t c = (ip >> 8) & 0xFFu;
    const uint32_t d = ip & 0xFFu;

    (void)snprintf(out, 16, "%u.%u.%u.%u", a, b, c, d);
}

static bool parse_ipv4(const char* s, uint32_t& out_ip_be) {
    if (!s || !*s) {
        return false;
    }

    uint32_t parts[4] = { 0, 0, 0, 0 };
    uint32_t idx = 0;
    uint32_t val = 0;
    bool any = false;

    for (;;) {
        const char c = *s;
        if (c >= '0' && c <= '9') {
            any = true;
            val = (val * 10u) + (uint32_t)(c - '0');
            if (val > 255u) {
                return false;
            }
            s++;
            continue;
        }

        if (c == '.' || c == '\0') {
            if (!any || idx >= 4u) {
                return false;
            }

            parts[idx] = val;
            idx++;

            val = 0;
            any = false;

            if (c == '\0') {
                break;
            }

            s++;
            continue;
        }

        return false;
    }

    if (idx != 4u) {
        return false;
    }

    const uint32_t ip = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
    out_ip_be = htonl(ip);
    return true;
}

static int write_all(int fd, const void* buf, uint32_t size) {
    const uint8_t* p = (const uint8_t*)buf;

    uint32_t done = 0;
    while (done < size) {
        const int r = write(fd, p + done, size - done);
        if (r <= 0) {
            return -1;
        }
        done += (uint32_t)r;
    }

    return 0;
}

static int read_all_timeout(int fd, void* buf, uint32_t size, uint32_t timeout_ms) {
    uint8_t* p = (uint8_t*)buf;

    uint32_t done = 0;
    const uint32_t start = uptime_ms();

    while (done < size) {
        const uint32_t now = uptime_ms();
        if ((now - start) >= timeout_ms) {
            return 0;
        }

        pollfd_t fds[1];
        fds[0].fd = fd;
        fds[0].events = POLLIN;
        fds[0].revents = 0;

        const int pr = poll(fds, 1, 10);
        if (pr <= 0) {
            continue;
        }

        const int r = read(fd, p + done, size - done);
        if (r <= 0) {
            return -1;
        }
        done += (uint32_t)r;
    }

    return 1;
}

struct Options {
    uint32_t dst_ip_be;
    uint32_t count;
    uint32_t timeout_ms;
    uint32_t interval_ms;

    uint8_t name_len;
    char name[127];
};

static void print_usage() {
    printf("usage: ping <ip|name> [-c count] [-W timeout_ms]\n");
}

static bool parse_options(int argc, char** argv, Options& out) {
    out.dst_ip_be = 0;
    out.count = kDefaultCount;
    out.timeout_ms = kDefaultTimeoutMs;
    out.interval_ms = kDefaultIntervalMs;
    out.name_len = 0;

    const char* target_str = 0;

    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        if (!a || !*a) {
            continue;
        }

        if (strcmp(a, "-c") == 0) {
            if ((i + 1) >= argc) {
                return false;
            }

            const int v = atoi(argv[i + 1]);
            if (v > 0) {
                out.count = (uint32_t)v;
            }

            i++;
            continue;
        }

        if (strcmp(a, "-W") == 0) {
            if ((i + 1) >= argc) {
                return false;
            }

            const int v = atoi(argv[i + 1]);
            if (v > 0) {
                out.timeout_ms = (uint32_t)v;
            }

            i++;
            continue;
        }

        if (!target_str) {
            target_str = a;
            continue;
        }

        return false;
    }

    if (!target_str) {
        return false;
    }

    if (parse_ipv4(target_str, out.dst_ip_be)) {
        return true;
    }

    uint32_t n = 0;
    for (const char* p = target_str; *p; p++) {
        n++;
        if (n > sizeof(out.name)) {
            return false;
        }
    }

    if (n == 0u) {
        return false;
    }

    out.name_len = (uint8_t)n;
    memcpy(out.name, target_str, n);
    return true;
}

static uint64_t isqrt_u64(uint64_t x) {
    uint64_t r = 0;
    uint64_t bit = 1ull << 62;

    while (bit > x) {
        bit >>= 2;
    }

    while (bit != 0) {
        const uint64_t t = r + bit;
        if (x >= t) {
            x -= t;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }

    return r;
}

struct RttStats {
    uint32_t received;
    uint32_t min_ms;
    uint32_t max_ms;
    uint64_t sum_us;
    uint64_t sumsq_us;

    void reset() {
        received = 0;
        min_ms = 0;
        max_ms = 0;
        sum_us = 0;
        sumsq_us = 0;
    }

    void add_ms(uint32_t rtt_ms) {
        if (received == 0) {
            min_ms = rtt_ms;
            max_ms = rtt_ms;
        } else {
            if (rtt_ms < min_ms) {
                min_ms = rtt_ms;
            }

            if (rtt_ms > max_ms) {
                max_ms = rtt_ms;
            }
        }

        const uint64_t us = (uint64_t)rtt_ms * 1000ull;
        sum_us += us;
        sumsq_us += us * us;
        received++;
    }

    uint32_t avg_us() const {
        if (received == 0) {
            return 0;
        }
        return (uint32_t)(sum_us / (uint64_t)received);
    }

    uint32_t mdev_us() const {
        if (received < 2) {
            return 0;
        }

        const uint64_t n = (uint64_t)received;
        const uint64_t mean = sum_us / n;
        const uint64_t mean_sq = (mean * mean);
        const uint64_t ex2 = sumsq_us / n;

        uint64_t var = 0;
        if (ex2 > mean_sq) {
            var = ex2 - mean_sq;
        }

        return (uint32_t)isqrt_u64(var);
    }
};

static void print_time_us(uint32_t us) {
    const uint32_t ms = us / 1000u;
    const uint32_t frac = us % 1000u;

    printf("%u.%03u", ms, frac);
}

static int connect_networkd(int& out_r, int& out_w) {
    int fds[2] = { -1, -1 };
    if (ipc_connect("networkd", fds) != 0) {
        return -1;
    }

    out_r = fds[0];
    out_w = fds[1];
    return 0;
}

static void close_fds(int& fd_r, int& fd_w) {
    if (fd_r >= 0) {
        close(fd_r);
        fd_r = -1;
    }

    if (fd_w >= 0) {
        close(fd_w);
        fd_w = -1;
    }
}

static int recv_ipc_reply_hdr(int fd_r, uint32_t rx_timeout_ms, netd_ipc_hdr_t& out_hdr) {
    const int hr = read_all_timeout(fd_r, &out_hdr, (uint32_t)sizeof(out_hdr), rx_timeout_ms);
    if (hr <= 0) {
        return hr;
    }

    if (out_hdr.magic != NETD_IPC_MAGIC || out_hdr.version != NETD_IPC_VERSION) {
        return -1;
    }

    return 1;
}

static int drain_unknown_payload(int fd_r, uint32_t rx_timeout_ms, uint32_t len) {
    if (len == 0u) {
        return 0;
    }

    uint8_t trash[NETD_IPC_MAX_PAYLOAD];

    const uint32_t to_read = (len <= sizeof(trash)) ? len : (uint32_t)sizeof(trash);
    (void)read_all_timeout(fd_r, trash, to_read, rx_timeout_ms);
    return 0;
}

static int send_resolve_req(int fd_w, const Options& opt, uint32_t seq) {
    if (opt.name_len == 0u) {
        return -1;
    }

    netd_ipc_hdr_t hdr{};

    hdr.magic = NETD_IPC_MAGIC;
    hdr.version = NETD_IPC_VERSION;
    hdr.type = NETD_IPC_MSG_RESOLVE_REQ;
    hdr.len = (uint32_t)sizeof(netd_ipc_resolve_req_t);
    hdr.seq = seq;

    netd_ipc_resolve_req_t req{};
    req.name_len = opt.name_len;
    memcpy(req.name, opt.name, opt.name_len);
    req.timeout_ms = opt.timeout_ms;

    if (write_all(fd_w, &hdr, (uint32_t)sizeof(hdr)) != 0) {
        return -1;
    }

    if (write_all(fd_w, &req, (uint32_t)sizeof(req)) != 0) {
        return -1;
    }

    return 0;
}

static int recv_resolve_rsp(int fd_r, uint32_t rx_timeout_ms, netd_ipc_resolve_rsp_t& out) {
    netd_ipc_hdr_t hdr{};

    const int hr = recv_ipc_reply_hdr(fd_r, rx_timeout_ms, hdr);
    if (hr <= 0) {
        return hr;
    }

    if (hdr.type != NETD_IPC_MSG_RESOLVE_RSP || hdr.len != sizeof(netd_ipc_resolve_rsp_t)) {
        (void)drain_unknown_payload(fd_r, rx_timeout_ms, hdr.len);
        return -1;
    }

    return read_all_timeout(fd_r, &out, (uint32_t)sizeof(out), rx_timeout_ms);
}

static int send_ping_req(int fd_w, uint32_t dst_ip_be, uint16_t ident_host, uint16_t seq_host, uint32_t timeout_ms) {
    netd_ipc_hdr_t hdr{};

    hdr.magic = NETD_IPC_MAGIC;
    hdr.version = NETD_IPC_VERSION;
    hdr.type = NETD_IPC_MSG_PING_REQ;
    hdr.len = (uint32_t)sizeof(netd_ipc_ping_req_t);
    hdr.seq = (uint32_t)seq_host;

    netd_ipc_ping_req_t req{};

    req.dst_ip_be = dst_ip_be;
    req.ident_be = htons(ident_host);
    req.seq_be = htons(seq_host);
    req.timeout_ms = timeout_ms;

    if (write_all(fd_w, &hdr, (uint32_t)sizeof(hdr)) != 0) {
        return -1;
    }

    if (write_all(fd_w, &req, (uint32_t)sizeof(req)) != 0) {
        return -1;
    }

    return 0;
}

static int recv_ping_rsp(int fd_r, uint32_t rx_timeout_ms, netd_ipc_ping_rsp_t& out) {
    netd_ipc_hdr_t hdr{};

    const int hr = recv_ipc_reply_hdr(fd_r, rx_timeout_ms, hdr);
    if (hr <= 0) {
        return hr;
    }

    if (hdr.type != NETD_IPC_MSG_PING_RSP || hdr.len != sizeof(netd_ipc_ping_rsp_t)) {
        (void)drain_unknown_payload(fd_r, rx_timeout_ms, hdr.len);
        return -1;
    }

    return read_all_timeout(fd_r, &out, (uint32_t)sizeof(out), rx_timeout_ms);
}

static void print_header(const Options& opt) {
    char ip[16];

    ip_to_string(opt.dst_ip_be, ip);

    const uint32_t total = kPayloadBytes + 28u;
    printf("PING %s (%s) %u(%u) bytes of data.\n", ip, ip, kPayloadBytes, total);
}

static void print_reply_line(uint32_t seq, uint32_t rtt_ms, const Options& opt) {
    char ip[16];
    ip_to_string(opt.dst_ip_be, ip);

    printf(
        "%u bytes from %s: icmp_seq=%u ttl=%u time=%u.000 ms\n",
        kReplyLineBytes,
        ip,
        seq,
        kTtl,
        rtt_ms
    );
}

static void print_timeout_line() {
    printf("timeout\n");
}

static void print_summary(const Options& opt, uint32_t transmitted, uint32_t received, uint32_t time_ms, const RttStats& stats) {
    char ip[16];
    ip_to_string(opt.dst_ip_be, ip);

    printf("\n");
    printf("--- %s ping statistics ---\n", ip);

    uint32_t loss = 0u;
    if (transmitted != 0u) {
        const uint64_t lost = (uint64_t)(transmitted - received);
        loss = (uint32_t)((lost * 100ull) / (uint64_t)transmitted);
    }

    printf(
        "%u packets transmitted, %u received, %u%% packet loss, time %ums\n",
        transmitted,
        received,
        loss,
        time_ms
    );

    if (received == 0) {
        return;
    }

    const uint32_t min_us = stats.min_ms * 1000u;
    const uint32_t avg_us = stats.avg_us();
    const uint32_t max_us = stats.max_ms * 1000u;
    const uint32_t mdev_us = stats.mdev_us();

    printf("rtt min/avg/max/mdev = ");
    print_time_us(min_us);
    printf("/");
    print_time_us(avg_us);
    printf("/");
    print_time_us(max_us);
    printf("/");
    print_time_us(mdev_us);
    printf(" ms\n");
}

}

extern "C" int main(int argc, char** argv) {

    ping::Options opt{};

    if (!ping::parse_options(argc, argv, opt)) {
        ping::print_usage();
        return 1;
    }

    int fd_r = -1;
    int fd_w = -1;

    if (ping::connect_networkd(fd_r, fd_w) != 0) {
        printf("ping: networkd not running\n");
        return 1;
    }

    if (opt.dst_ip_be == 0u) {
        if (ping::send_resolve_req(fd_w, opt, 1u) != 0) {
            printf("ping: resolve send failed\n");
            ping::close_fds(fd_r, fd_w);
            return 1;
        }

        netd_ipc_resolve_rsp_t rsp{};
        const uint32_t rx_timeout_ms = opt.timeout_ms + 1500u;
        const int rr = ping::recv_resolve_rsp(fd_r, rx_timeout_ms, rsp);
        if (rr <= 0 || rsp.ok == 0u || rsp.ip_be == 0u) {
            printf("ping: resolve failed\n");
            ping::close_fds(fd_r, fd_w);
            return 1;
        }

        opt.dst_ip_be = rsp.ip_be;
    }

    ping::print_header(opt);

    const uint16_t ident = (uint16_t)(getpid() & 0xFFFF);
    const uint32_t start_ms = uptime_ms();

    uint32_t transmitted = 0;
    uint32_t received = 0;

    ping::RttStats stats{};
    stats.reset();

    for (uint32_t seq = 1; seq <= opt.count; seq++) {
        transmitted++;

        if (ping::send_ping_req(fd_w, opt.dst_ip_be, ident, (uint16_t)seq, opt.timeout_ms) != 0) {
            printf("ping: send failed\n");
            ping::close_fds(fd_r, fd_w);
            return 1;
        }

        netd_ipc_ping_rsp_t rsp{};
        const uint32_t rx_timeout_ms = opt.timeout_ms + 1500u;
        const int rr = ping::recv_ping_rsp(fd_r, rx_timeout_ms, rsp);

        if (rr <= 0 || rsp.ok == 0u) {
            ping::print_timeout_line();
        } else {
            received++;
            stats.add_ms(rsp.rtt_ms);
            ping::print_reply_line(seq, rsp.rtt_ms, opt);
        }

        if (seq != opt.count) {
            sleep((int)opt.interval_ms);
        }
    }

    const uint32_t total_ms = uptime_ms() - start_ms;
    ping::print_summary(opt, transmitted, received, total_ms, stats);

    ping::close_fds(fd_r, fd_w);
    return 0;
}
