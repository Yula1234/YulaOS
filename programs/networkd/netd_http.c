// SPDX-License-Identifier: GPL-2.0

#include "netd_http.h"

#include <yula.h>

#include "netd_dns.h"
#include "netd_tcp.h"
#include "netd_tls.h"

#define NETD_HTTP_MAX_REDIRECTS 4u

static int netd_http_is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static char netd_http_tolower(char c) {
    if (c >= 'A' && c <= 'Z') {
        return (char)(c - 'A' + 'a');
    }
    return c;
}

static int netd_http_ieq_n(const char* a, const char* b, uint32_t n) {
    if (!a || !b) {
        return 0;
    }

    for (uint32_t i = 0; i < n; i++) {
        char ca = netd_http_tolower(a[i]);
        char cb = netd_http_tolower(b[i]);
        if (ca != cb) {
            return 0;
        }
        if (ca == '\0') {
            return 1;
        }
    }

    return 1;
}

static int netd_http_ieq(const char* a, const char* b) {
    if (!a || !b) {
        return 0;
    }

    uint32_t i = 0;
    for (;;) {
        char ca = netd_http_tolower(a[i]);
        char cb = netd_http_tolower(b[i]);
        if (ca != cb) {
            return 0;
        }
        if (ca == '\0') {
            return 1;
        }
        i++;
    }
}

static void netd_http_trim(char** io_s) {
    if (!io_s || !*io_s) {
        return;
    }

    char* s = *io_s;
    while (*s && netd_http_is_space(*s)) {
        s++;
    }
    *io_s = s;
}

static int netd_http_parse_u32(const char* s, uint32_t* out) {
    if (!s || !out) {
        return 0;
    }

    uint32_t v = 0;
    int any = 0;
    for (const char* p = s; *p; p++) {
        if (*p < '0' || *p > '9') {
            break;
        }
        any = 1;
        uint32_t d = (uint32_t)(*p - '0');
        if (v > (0xFFFFFFFFu - d) / 10u) {
            return 0;
        }
        v = v * 10u + d;
    }

    if (!any) {
        return 0;
    }

    *out = v;
    return 1;
}

static int netd_http_find_crlfcrlf(const uint8_t* buf, uint32_t len, uint32_t* out_off) {
    if (!buf || !out_off) {
        return 0;
    }

    if (len < 4u) {
        return 0;
    }

    for (uint32_t i = 0; i + 3u < len; i++) {
        if (buf[i] == '\r' && buf[i + 1u] == '\n' && buf[i + 2u] == '\r' && buf[i + 3u] == '\n') {
            *out_off = i + 4u;
            return 1;
        }
    }

    return 0;
}

static int netd_http_parse_url(
    const char* url,
    char* host,
    uint32_t host_cap,
    uint16_t* out_port,
    char* path,
    uint32_t path_cap,
    int* out_is_https
) {
    if (out_is_https) {
        *out_is_https = 0;
    }

    if (!url || !*url || !host || host_cap == 0 || !out_port || !path || path_cap == 0) {
        return 0;
    }

    const char* s = url;
    int is_https = 0;
    uint16_t default_port = 80u;
    if (netd_http_ieq_n(s, "http://", 7u)) {
        s += 7;
    } else if (netd_http_ieq_n(s, "https://", 8u)) {
        s += 8;
        is_https = 1;
        default_port = 443u;
    }

    while (*s == '/') {
        s++;
    }

    const char* host_start = s;
    const char* host_end = s;
    while (*host_end && *host_end != '/') {
        host_end++;
    }

    uint32_t host_len = (uint32_t)(host_end - host_start);
    if (host_len == 0) {
        return 0;
    }

    const char* colon = 0;
    for (const char* p = host_start; p < host_end; p++) {
        if (*p == ':') {
            colon = p;
            break;
        }
    }

    uint16_t port = default_port;
    uint32_t name_len = host_len;
    if (colon) {
        name_len = (uint32_t)(colon - host_start);
        const char* port_str = colon + 1;
        uint32_t port_u32 = 0;
        if (!netd_http_parse_u32(port_str, &port_u32) || port_u32 == 0 || port_u32 > 65535u) {
            return 0;
        }
        port = (uint16_t)port_u32;
    }

    if (name_len + 1u > host_cap) {
        return 0;
    }

    memcpy(host, host_start, name_len);
    host[name_len] = '\0';

    const char* path_start = host_end;
    if (*path_start == '\0') {
        if (path_cap < 2u) {
            return 0;
        }
        path[0] = '/';
        path[1] = '\0';
    } else {
        uint32_t p_len = (uint32_t)strlen(path_start);
        if (p_len + 1u > path_cap) {
            return 0;
        }
        memcpy(path, path_start, p_len);
        path[p_len] = '\0';
    }

    *out_port = port;
    if (out_is_https) {
        *out_is_https = is_https;
    }
    return 1;
}

static void netd_http_send_begin(int fd_out, uint32_t seq, uint32_t status, uint32_t http_status, uint32_t content_length) {
    net_http_get_begin_t begin;
    begin.status = status;
    begin.http_status = http_status;
    begin.content_length = content_length;
    begin.flags = 0;
    (void)net_ipc_send(fd_out, NET_IPC_MSG_HTTP_GET_BEGIN, seq, &begin, (uint32_t)sizeof(begin));
}

static void netd_http_send_stage(int fd_out, uint32_t seq, uint32_t stage, uint32_t status, uint32_t detail, uint32_t flags) {
    net_http_get_stage_t msg;
    msg.stage = stage;
    msg.status = status;
    msg.detail = detail;
    msg.flags = flags;
    (void)net_ipc_send(fd_out, NET_IPC_MSG_HTTP_GET_STAGE, seq, &msg, (uint32_t)sizeof(msg));
}

static void netd_http_send_end(int fd_out, uint32_t seq, uint32_t status) {
    net_http_get_end_t end;
    end.status = status;
    (void)net_ipc_send(fd_out, NET_IPC_MSG_HTTP_GET_END, seq, &end, (uint32_t)sizeof(end));
}

static void netd_http_send_data(int fd_out, uint32_t seq, const uint8_t* data, uint32_t len) {
    if (!data || len == 0) {
        return;
    }

    while (len > 0) {
        uint32_t chunk = len;
        if (chunk > NET_IPC_MAX_PAYLOAD) {
            chunk = NET_IPC_MAX_PAYLOAD;
        }

        (void)net_ipc_send(fd_out, NET_IPC_MSG_HTTP_GET_DATA, seq, data, chunk);
        data += chunk;
        len -= chunk;
    }
}

typedef struct {
    int use_tls;
    netd_tls_client_t tls;
} netd_http_io_t;

static int netd_http_io_send(netd_ctx_t* ctx, netd_http_io_t* io, const void* data, uint32_t len, uint32_t timeout_ms) {
    if (!ctx || !io) {
        return 0;
    }

    if (io->use_tls) {
        return netd_tls_send(ctx, &io->tls, data, len, timeout_ms);
    }

    return netd_tcp_send(ctx, data, len, timeout_ms);
}

static int netd_http_io_recv(netd_ctx_t* ctx, netd_http_io_t* io, void* out, uint32_t cap, uint32_t timeout_ms, uint32_t* out_n) {
    if (!ctx || !io) {
        return 0;
    }

    if (io->use_tls) {
        return netd_tls_recv(ctx, &io->tls, out, cap, timeout_ms, out_n);
    }

    return netd_tcp_recv(ctx, out, cap, timeout_ms, out_n);
}

static int netd_http_io_close(netd_ctx_t* ctx, netd_http_io_t* io, uint32_t timeout_ms) {
    if (!ctx || !io) {
        return 0;
    }

    if (io->use_tls) {
        return netd_tls_close(ctx, &io->tls, timeout_ms);
    }

    return netd_tcp_close(ctx, timeout_ms);
}

static int netd_http_read_some(netd_ctx_t* ctx, netd_http_io_t* io, uint8_t* out, uint32_t cap, uint32_t timeout_ms, uint32_t* out_n) {
    if (out_n) {
        *out_n = 0;
    }

    if (!ctx || !out || cap == 0) {
        return 0;
    }

    uint32_t got = 0;
    if (!netd_http_io_recv(ctx, io, out, cap, timeout_ms, &got)) {
        return 0;
    }

    if (out_n) {
        *out_n = got;
    }
    return 1;
}

typedef struct {
    const uint8_t* buf;
    uint32_t r;
    uint32_t w;
} netd_http_prefetch_t;

static int netd_http_read_some_pf(
    netd_ctx_t* ctx,
    netd_http_io_t* io,
    netd_http_prefetch_t* pf,
    uint8_t* out,
    uint32_t cap,
    uint32_t timeout_ms,
    uint32_t* out_n
) {
    if (out_n) {
        *out_n = 0;
    }

    if (!ctx || !out || cap == 0) {
        return 0;
    }

    if (pf && pf->buf && pf->r < pf->w) {
        uint32_t avail = pf->w - pf->r;
        if (cap > avail) {
            cap = avail;
        }
        memcpy(out, pf->buf + pf->r, cap);
        pf->r += cap;
        if (out_n) {
            *out_n = cap;
        }
        return 1;
    }

    return netd_http_read_some(ctx, io, out, cap, timeout_ms, out_n);
}

static int netd_http_parse_status_line(const uint8_t* hdr, uint32_t hdr_len, uint32_t* out_status) {
    if (!hdr || hdr_len == 0 || !out_status) {
        return 0;
    }

    const char* s = (const char*)hdr;
    const char* end = (const char*)hdr + hdr_len;

    const char* line_end = s;
    while (line_end < end) {
        if (line_end + 1 < end && line_end[0] == '\r' && line_end[1] == '\n') {
            break;
        }
        line_end++;
    }

    if (line_end >= end) {
        return 0;
    }

    const char* sp = s;
    while (sp < line_end && *sp != ' ') {
        sp++;
    }
    if (sp >= line_end) {
        return 0;
    }

    while (sp < line_end && *sp == ' ') {
        sp++;
    }
    if (sp >= line_end) {
        return 0;
    }

    uint32_t code = 0;
    if (!netd_http_parse_u32(sp, &code)) {
        return 0;
    }

    *out_status = code;
    return 1;
}

static int netd_http_parse_headers(
    uint8_t* hdr,
    uint32_t hdr_len,
    uint32_t* out_content_length,
    int* out_chunked,
    char* location,
    uint32_t location_cap
) {
    if (!hdr || hdr_len == 0) {
        return 0;
    }

    if (out_content_length) {
        *out_content_length = 0;
    }
    if (out_chunked) {
        *out_chunked = 0;
    }
    if (location && location_cap > 0) {
        location[0] = '\0';
    }

    uint8_t* p = hdr;
    uint8_t* end = hdr + hdr_len;

    while (p < end) {
        if (p + 1u < end && p[0] == '\r' && p[1] == '\n') {
            return 1;
        }

        uint8_t* line = p;
        uint8_t* line_end = p;
        while (line_end < end) {
            if (line_end + 1u < end && line_end[0] == '\r' && line_end[1] == '\n') {
                break;
            }
            line_end++;
        }

        if (line_end >= end) {
            return 0;
        }

        *line_end = '\0';

        char* s = (char*)line;
        char* colon = s;
        while (*colon && *colon != ':') {
            colon++;
        }

        if (*colon == ':') {
            *colon = '\0';
            char* name = s;
            char* value = colon + 1;
            netd_http_trim(&value);

            if (out_content_length && netd_http_ieq(name, "Content-Length")) {
                uint32_t v = 0;
                if (netd_http_parse_u32(value, &v)) {
                    *out_content_length = v;
                }
            }

            if (out_chunked && netd_http_ieq(name, "Transfer-Encoding")) {
                if (value && netd_http_ieq(value, "chunked")) {
                    *out_chunked = 1;
                }
            }

            if (location && location_cap > 0 && netd_http_ieq(name, "Location")) {
                uint32_t vlen = (uint32_t)strlen(value);
                if (vlen + 1u < location_cap) {
                    memcpy(location, value, vlen);
                    location[vlen] = '\0';
                }
            }
        }

        p = line_end + 2u;
    }

    return 0;
}

static int netd_http_parse_hex_u32(const char* s, uint32_t* out) {
    if (!s || !out) {
        return 0;
    }

    uint32_t v = 0;
    int any = 0;

    for (const char* p = s; *p; p++) {
        char c = *p;
        if (c == ';' || c == '\r' || c == '\n' || c == ' ') {
            break;
        }

        uint32_t d = 0;
        if (c >= '0' && c <= '9') {
            d = (uint32_t)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            d = 10u + (uint32_t)(c - 'a');
        } else if (c >= 'A' && c <= 'F') {
            d = 10u + (uint32_t)(c - 'A');
        } else {
            return 0;
        }

        any = 1;
        if (v > (0xFFFFFFFFu - d) / 16u) {
            return 0;
        }
        v = v * 16u + d;
    }

    if (!any) {
        return 0;
    }

    *out = v;
    return 1;
}

static int netd_http_read_line(netd_ctx_t* ctx, netd_http_io_t* io, netd_http_prefetch_t* pf, char* line, uint32_t cap, uint32_t timeout_ms) {
    if (!ctx || !line || cap < 2u) {
        return 0;
    }

    uint32_t n = 0;
    for (;;) {
        if (n + 1u >= cap) {
            return 0;
        }

        uint8_t b = 0;
        uint32_t got = 0;
        if (!netd_http_read_some_pf(ctx, io, pf, &b, 1u, timeout_ms, &got)) {
            return 0;
        }
        if (got == 0) {
            return 0;
        }

        line[n] = (char)b;
        n++;
        line[n] = '\0';

        if (n >= 2u && line[n - 2u] == '\r' && line[n - 1u] == '\n') {
            line[n - 2u] = '\0';
            return 1;
        }
    }
}

static int netd_http_read_exact(netd_ctx_t* ctx, netd_http_io_t* io, netd_http_prefetch_t* pf, uint8_t* buf, uint32_t n, uint32_t timeout_ms) {
    if (!ctx || (!buf && n != 0)) {
        return 0;
    }

    uint32_t off = 0;
    while (off < n) {
        uint32_t got = 0;
        uint32_t cap = n - off;
        if (cap > 256u) {
            cap = 256u;
        }
        if (!netd_http_read_some_pf(ctx, io, pf, buf + off, cap, timeout_ms, &got)) {
            return 0;
        }
        if (got == 0) {
            return 0;
        }
        off += got;
    }
    return 1;
}

static int netd_http_drain_crlf(netd_ctx_t* ctx, netd_http_io_t* io, netd_http_prefetch_t* pf, uint32_t timeout_ms) {
    uint8_t crlf[2];
    if (!netd_http_read_exact(ctx, io, pf, crlf, 2u, timeout_ms)) {
        return 0;
    }
    if (crlf[0] != '\r' || crlf[1] != '\n') {
        return 0;
    }
    return 1;
}

static uint32_t netd_http_do_get_one(netd_ctx_t* ctx, int fd_out, uint32_t seq, const char* url, uint32_t timeout_ms, uint32_t redirects_left) {
    char host[256];
    char path[512];
    uint16_t port = 80u;
    int is_https = 0;

    netd_http_send_stage(fd_out, seq, NET_HTTP_GET_STAGE_PARSE_URL, NET_STATUS_OK, 0, NET_HTTP_GET_STAGE_F_BEGIN);
    if (!netd_http_parse_url(url, host, (uint32_t)sizeof(host), &port, path, (uint32_t)sizeof(path), &is_https)) {
        netd_http_send_stage(fd_out, seq, NET_HTTP_GET_STAGE_PARSE_URL, NET_STATUS_UNSUPPORTED, 0, NET_HTTP_GET_STAGE_F_END);
        netd_http_send_begin(fd_out, seq, NET_STATUS_UNSUPPORTED, 0, 0);
        netd_http_send_end(fd_out, seq, NET_STATUS_UNSUPPORTED);
        return NET_STATUS_UNSUPPORTED;
    }

    netd_http_send_stage(fd_out, seq, NET_HTTP_GET_STAGE_PARSE_URL, NET_STATUS_OK, (uint32_t)is_https, NET_HTTP_GET_STAGE_F_END);

    uint32_t ip = 0;
    netd_http_send_stage(fd_out, seq, NET_HTTP_GET_STAGE_DNS, NET_STATUS_OK, 0, NET_HTTP_GET_STAGE_F_BEGIN);
    if (!netd_dns_query(ctx, host, timeout_ms, &ip)) {
        netd_http_send_stage(fd_out, seq, NET_HTTP_GET_STAGE_DNS, NET_STATUS_TIMEOUT, 0, NET_HTTP_GET_STAGE_F_END);
        netd_http_send_begin(fd_out, seq, NET_STATUS_TIMEOUT, 0, 0);
        netd_http_send_end(fd_out, seq, NET_STATUS_TIMEOUT);
        return NET_STATUS_TIMEOUT;
    }
    netd_http_send_stage(fd_out, seq, NET_HTTP_GET_STAGE_DNS, NET_STATUS_OK, ip, NET_HTTP_GET_STAGE_F_END);

    netd_http_io_t io;
    memset(&io, 0, sizeof(io));
    io.use_tls = is_https;

    netd_http_send_stage(fd_out, seq, NET_HTTP_GET_STAGE_CONNECT, NET_STATUS_OK, port, NET_HTTP_GET_STAGE_F_BEGIN);
    if (!netd_tcp_connect(ctx, ip, port, timeout_ms)) {
        uint32_t st = ctx->tcp.last_err;
        if (st == NET_STATUS_OK || st == 0) {
            st = NET_STATUS_TIMEOUT;
        }
        netd_http_send_stage(fd_out, seq, NET_HTTP_GET_STAGE_CONNECT, st, port, NET_HTTP_GET_STAGE_F_END);
        netd_http_send_begin(fd_out, seq, st, 0, 0);
        netd_http_send_end(fd_out, seq, st);
        return st;
    }
    netd_http_send_stage(fd_out, seq, NET_HTTP_GET_STAGE_CONNECT, NET_STATUS_OK, port, NET_HTTP_GET_STAGE_F_END);

    if (io.use_tls) {
        netd_http_send_stage(fd_out, seq, NET_HTTP_GET_STAGE_TLS_HANDSHAKE, NET_STATUS_OK, 0, NET_HTTP_GET_STAGE_F_BEGIN);
        if (!netd_tls_handshake(ctx, &io.tls, host, timeout_ms)) {
            (void)netd_tcp_close(ctx, timeout_ms);
            uint32_t hs_status = io.tls.hs_status;
            if (hs_status == NET_STATUS_OK) {
                hs_status = NET_STATUS_ERROR;
            }
            uint32_t detail = NET_HTTP_TLS_DETAIL_MAKE(io.tls.hs_step, io.tls.hs_alert);
            netd_http_send_stage(fd_out, seq, NET_HTTP_GET_STAGE_TLS_HANDSHAKE, hs_status, detail, NET_HTTP_GET_STAGE_F_END);
            netd_http_send_begin(fd_out, seq, hs_status, 0, 0);
            netd_http_send_end(fd_out, seq, hs_status);
            return hs_status;
        }
        netd_http_send_stage(fd_out, seq, NET_HTTP_GET_STAGE_TLS_HANDSHAKE, NET_STATUS_OK, 0, NET_HTTP_GET_STAGE_F_END);
    }

    char req_buf[1024];
    int rn = snprintf(
        req_buf,
        sizeof(req_buf),
        "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: yulaos-wget/1\r\nConnection: close\r\n\r\n",
        path,
        host
    );

    if (rn <= 0 || (uint32_t)rn >= (uint32_t)sizeof(req_buf)) {
        (void)netd_http_io_close(ctx, &io, timeout_ms);
        netd_http_send_begin(fd_out, seq, NET_STATUS_ERROR, 0, 0);
        netd_http_send_end(fd_out, seq, NET_STATUS_ERROR);
        return NET_STATUS_ERROR;
    }

    netd_http_send_stage(fd_out, seq, NET_HTTP_GET_STAGE_SEND_REQUEST, NET_STATUS_OK, 0, NET_HTTP_GET_STAGE_F_BEGIN);
    if (!netd_http_io_send(ctx, &io, req_buf, (uint32_t)rn, timeout_ms)) {
        (void)netd_http_io_close(ctx, &io, timeout_ms);
        netd_http_send_stage(fd_out, seq, NET_HTTP_GET_STAGE_SEND_REQUEST, NET_STATUS_TIMEOUT, 0, NET_HTTP_GET_STAGE_F_END);
        netd_http_send_begin(fd_out, seq, NET_STATUS_TIMEOUT, 0, 0);
        netd_http_send_end(fd_out, seq, NET_STATUS_TIMEOUT);
        return NET_STATUS_TIMEOUT;
    }
    netd_http_send_stage(fd_out, seq, NET_HTTP_GET_STAGE_SEND_REQUEST, NET_STATUS_OK, 0, NET_HTTP_GET_STAGE_F_END);

    uint8_t hdr_buf[2048];
    uint32_t hdr_w = 0;
    uint32_t body_off = 0;
    netd_http_send_stage(fd_out, seq, NET_HTTP_GET_STAGE_RECV_HEADERS, NET_STATUS_OK, 0, NET_HTTP_GET_STAGE_F_BEGIN);
    while (hdr_w < (uint32_t)sizeof(hdr_buf)) {
        uint32_t got = 0;
        if (!netd_http_read_some(ctx, &io, hdr_buf + hdr_w, (uint32_t)sizeof(hdr_buf) - hdr_w, timeout_ms, &got)) {
            (void)netd_http_io_close(ctx, &io, timeout_ms);
            netd_http_send_stage(fd_out, seq, NET_HTTP_GET_STAGE_RECV_HEADERS, NET_STATUS_TIMEOUT, 0, NET_HTTP_GET_STAGE_F_END);
            netd_http_send_begin(fd_out, seq, NET_STATUS_TIMEOUT, 0, 0);
            netd_http_send_end(fd_out, seq, NET_STATUS_TIMEOUT);
            return NET_STATUS_TIMEOUT;
        }
        if (got == 0) {
            (void)netd_http_io_close(ctx, &io, timeout_ms);
            netd_http_send_stage(fd_out, seq, NET_HTTP_GET_STAGE_RECV_HEADERS, NET_STATUS_ERROR, 0, NET_HTTP_GET_STAGE_F_END);
            netd_http_send_begin(fd_out, seq, NET_STATUS_ERROR, 0, 0);
            netd_http_send_end(fd_out, seq, NET_STATUS_ERROR);
            return NET_STATUS_ERROR;
        }

        hdr_w += got;
        if (netd_http_find_crlfcrlf(hdr_buf, hdr_w, &body_off)) {
            break;
        }
    }

    if (body_off == 0) {
        (void)netd_http_io_close(ctx, &io, timeout_ms);
        netd_http_send_stage(fd_out, seq, NET_HTTP_GET_STAGE_RECV_HEADERS, NET_STATUS_ERROR, 0, NET_HTTP_GET_STAGE_F_END);
        netd_http_send_begin(fd_out, seq, NET_STATUS_ERROR, 0, 0);
        netd_http_send_end(fd_out, seq, NET_STATUS_ERROR);
        return NET_STATUS_ERROR;
    }

    uint32_t http_status = 0;
    if (!netd_http_parse_status_line(hdr_buf, body_off, &http_status)) {
        (void)netd_http_io_close(ctx, &io, timeout_ms);
        netd_http_send_stage(fd_out, seq, NET_HTTP_GET_STAGE_RECV_HEADERS, NET_STATUS_ERROR, 0, NET_HTTP_GET_STAGE_F_END);
        netd_http_send_begin(fd_out, seq, NET_STATUS_ERROR, 0, 0);
        netd_http_send_end(fd_out, seq, NET_STATUS_ERROR);
        return NET_STATUS_ERROR;
    }

    uint32_t content_length = 0;
    int chunked = 0;
    char location[384];

    if (!netd_http_parse_headers(hdr_buf, body_off, &content_length, &chunked, location, (uint32_t)sizeof(location))) {
        (void)netd_http_io_close(ctx, &io, timeout_ms);
        netd_http_send_stage(fd_out, seq, NET_HTTP_GET_STAGE_RECV_HEADERS, NET_STATUS_ERROR, http_status, NET_HTTP_GET_STAGE_F_END);
        netd_http_send_begin(fd_out, seq, NET_STATUS_ERROR, http_status, 0);
        netd_http_send_end(fd_out, seq, NET_STATUS_ERROR);
        return NET_STATUS_ERROR;
    }

    netd_http_send_stage(fd_out, seq, NET_HTTP_GET_STAGE_RECV_HEADERS, NET_STATUS_OK, http_status, NET_HTTP_GET_STAGE_F_END);

    if ((http_status == 301u || http_status == 302u || http_status == 303u || http_status == 307u || http_status == 308u) && redirects_left > 0u) {
        if (location[0] != '\0') {
            (void)netd_http_io_close(ctx, &io, timeout_ms);
            return netd_http_do_get_one(ctx, fd_out, seq, location, timeout_ms, redirects_left - 1u);
        }
    }

    if (http_status < 200u || http_status >= 300u) {
        (void)netd_http_io_close(ctx, &io, timeout_ms);
        netd_http_send_begin(fd_out, seq, NET_STATUS_ERROR, http_status, 0);
        netd_http_send_end(fd_out, seq, NET_STATUS_ERROR);
        return NET_STATUS_ERROR;
    }

    netd_http_send_begin(fd_out, seq, NET_STATUS_OK, http_status, chunked ? 0u : content_length);
    netd_http_send_stage(fd_out, seq, NET_HTTP_GET_STAGE_RECV_BODY, NET_STATUS_OK, (uint32_t)chunked, NET_HTTP_GET_STAGE_F_BEGIN);

    netd_http_prefetch_t pf;
    pf.buf = hdr_buf + body_off;
    pf.r = 0;
    pf.w = (hdr_w > body_off) ? (hdr_w - body_off) : 0;

    if (!chunked) {
        uint32_t remaining = content_length;
        if (remaining == 0) {
            remaining = 0xFFFFFFFFu;
        }

        uint8_t buf[512];
        for (;;) {
            uint32_t cap = (uint32_t)sizeof(buf);
            if (remaining != 0xFFFFFFFFu && cap > remaining) {
                cap = remaining;
            }

            if (cap == 0) {
                break;
            }

            uint32_t got = 0;
            if (!netd_http_read_some_pf(ctx, &io, &pf, buf, cap, timeout_ms, &got)) {
                (void)netd_http_io_close(ctx, &io, timeout_ms);
                netd_http_send_stage(fd_out, seq, NET_HTTP_GET_STAGE_RECV_BODY, NET_STATUS_TIMEOUT, (uint32_t)chunked, NET_HTTP_GET_STAGE_F_END);
                netd_http_send_end(fd_out, seq, NET_STATUS_TIMEOUT);
                return NET_STATUS_TIMEOUT;
            }

            if (got == 0) {
                break;
            }

            netd_http_send_data(fd_out, seq, buf, got);

            if (remaining != 0xFFFFFFFFu) {
                if (got >= remaining) {
                    break;
                }
                remaining -= got;
            }
        }
    } else {
        char line[64];
        for (;;) {
            if (!netd_http_read_line(ctx, &io, &pf, line, (uint32_t)sizeof(line), timeout_ms)) {
                (void)netd_http_io_close(ctx, &io, timeout_ms);
                netd_http_send_stage(fd_out, seq, NET_HTTP_GET_STAGE_RECV_BODY, NET_STATUS_TIMEOUT, (uint32_t)chunked, NET_HTTP_GET_STAGE_F_END);
                netd_http_send_end(fd_out, seq, NET_STATUS_TIMEOUT);
                return NET_STATUS_TIMEOUT;
            }

            uint32_t chunk_size = 0;
            if (!netd_http_parse_hex_u32(line, &chunk_size)) {
                (void)netd_http_io_close(ctx, &io, timeout_ms);
                netd_http_send_stage(fd_out, seq, NET_HTTP_GET_STAGE_RECV_BODY, NET_STATUS_ERROR, (uint32_t)chunked, NET_HTTP_GET_STAGE_F_END);
                netd_http_send_end(fd_out, seq, NET_STATUS_ERROR);
                return NET_STATUS_ERROR;
            }

            if (chunk_size == 0) {
                for (;;) {
                    if (!netd_http_read_line(ctx, &io, &pf, line, (uint32_t)sizeof(line), timeout_ms)) {
                        (void)netd_http_io_close(ctx, &io, timeout_ms);
                        netd_http_send_stage(fd_out, seq, NET_HTTP_GET_STAGE_RECV_BODY, NET_STATUS_TIMEOUT, (uint32_t)chunked, NET_HTTP_GET_STAGE_F_END);
                        netd_http_send_end(fd_out, seq, NET_STATUS_TIMEOUT);
                        return NET_STATUS_TIMEOUT;
                    }
                    if (line[0] == '\0') {
                        break;
                    }
                }
                break;
            }

            uint8_t buf[512];
            uint32_t remaining = chunk_size;
            while (remaining > 0) {
                uint32_t cap = remaining;
                if (cap > (uint32_t)sizeof(buf)) {
                    cap = (uint32_t)sizeof(buf);
                }

                uint32_t got = 0;
                if (!netd_http_read_some_pf(ctx, &io, &pf, buf, cap, timeout_ms, &got)) {
                    (void)netd_http_io_close(ctx, &io, timeout_ms);
                    netd_http_send_stage(fd_out, seq, NET_HTTP_GET_STAGE_RECV_BODY, NET_STATUS_TIMEOUT, (uint32_t)chunked, NET_HTTP_GET_STAGE_F_END);
                    netd_http_send_end(fd_out, seq, NET_STATUS_TIMEOUT);
                    return NET_STATUS_TIMEOUT;
                }

                if (got == 0) {
                    (void)netd_http_io_close(ctx, &io, timeout_ms);
                    netd_http_send_stage(fd_out, seq, NET_HTTP_GET_STAGE_RECV_BODY, NET_STATUS_TIMEOUT, (uint32_t)chunked, NET_HTTP_GET_STAGE_F_END);
                    netd_http_send_end(fd_out, seq, NET_STATUS_TIMEOUT);
                    return NET_STATUS_TIMEOUT;
                }

                netd_http_send_data(fd_out, seq, buf, got);

                if (got >= remaining) {
                    remaining = 0;
                } else {
                    remaining -= got;
                }
            }

            if (!netd_http_drain_crlf(ctx, &io, &pf, timeout_ms)) {
                (void)netd_http_io_close(ctx, &io, timeout_ms);
                netd_http_send_stage(fd_out, seq, NET_HTTP_GET_STAGE_RECV_BODY, NET_STATUS_ERROR, (uint32_t)chunked, NET_HTTP_GET_STAGE_F_END);
                netd_http_send_end(fd_out, seq, NET_STATUS_ERROR);
                return NET_STATUS_ERROR;
            }
        }
    }

    (void)netd_http_io_close(ctx, &io, timeout_ms);
    netd_http_send_stage(fd_out, seq, NET_HTTP_GET_STAGE_RECV_BODY, NET_STATUS_OK, (uint32_t)chunked, NET_HTTP_GET_STAGE_F_END);
    netd_http_send_end(fd_out, seq, NET_STATUS_OK);
    return NET_STATUS_OK;
}

int netd_http_get(netd_ctx_t* ctx, int fd_out, uint32_t seq, const net_http_get_req_t* req) {
    if (!ctx || fd_out < 0 || !req) {
        return -1;
    }

    const char* url = req->url;
    uint32_t timeout_ms = req->timeout_ms;
    if (timeout_ms == 0) {
        timeout_ms = 5000u;
    }

    uint32_t status = netd_http_do_get_one(ctx, fd_out, seq, url, timeout_ms, NETD_HTTP_MAX_REDIRECTS);
    return status == NET_STATUS_OK ? 0 : -1;
}
