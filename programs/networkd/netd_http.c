// SPDX-License-Identifier: GPL-2.0

#include "netd_http.h"

#include <yula.h>

#include "netd_dns.h"
#include "netd_tcp.h"
#include "netd_tls.h"

#define NETD_HTTP_MAX_REDIRECTS 4u

#define NETD_HTTP_MAX_JOBS 8

static int netd_http_parse_url(
    const char* url,
    char* host,
    uint32_t host_cap,
    uint16_t* out_port,
    char* path,
    uint32_t path_cap,
    int* out_is_https
);

static int netd_http_parse_status_line(const uint8_t* buf, uint32_t len, uint32_t* out_status);

static int netd_http_parse_headers(
    uint8_t* hdr,
    uint32_t hdr_len,
    uint32_t* out_content_length,
    int* out_chunked,
    char* out_location,
    uint32_t location_cap
);

static int netd_http_parse_hex_u32(const char* s, uint32_t* out);

static void netd_http_send_begin(int fd_out, uint32_t seq, uint32_t status, uint32_t http_status, uint32_t content_length);
static void netd_http_send_stage(int fd_out, uint32_t seq, uint32_t stage, uint32_t status, uint32_t detail, uint32_t flags);
static void netd_http_send_end(int fd_out, uint32_t seq, uint32_t status);
static void netd_http_send_data(int fd_out, uint32_t seq, const uint8_t* data, uint32_t len);

typedef struct netd_http_job netd_http_job_t;

static int netd_http_is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static void netd_http_job_send_begin_once(netd_http_job_t* j, uint32_t status, uint32_t http_status, uint32_t content_length);

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

typedef enum {
    NETD_HTTP_JOB_PARSE_URL = 1,
    NETD_HTTP_JOB_DNS,
    NETD_HTTP_JOB_CONNECT,
    NETD_HTTP_JOB_SEND_REQ,
    NETD_HTTP_JOB_RECV_HDR,
    NETD_HTTP_JOB_RECV_BODY,
    NETD_HTTP_JOB_CLOSE,
    NETD_HTTP_JOB_DONE
} netd_http_job_stage_t;

typedef enum {
    NETD_HTTP_BODY_MODE_NONE = 0,
    NETD_HTTP_BODY_MODE_CL,
    NETD_HTTP_BODY_MODE_UNTIL_CLOSE,
    NETD_HTTP_BODY_MODE_CHUNK_SIZE,
    NETD_HTTP_BODY_MODE_CHUNK_DATA,
    NETD_HTTP_BODY_MODE_CHUNK_CRLF,
    NETD_HTTP_BODY_MODE_CHUNK_TRAILERS
} netd_http_body_mode_t;

struct netd_http_job {
    int active;
    int fd_out;
    uint32_t seq;

    int begin_sent;

    uint32_t timeout_ms;
    uint32_t start_ms;
    uint32_t stage_start_ms;
    netd_http_job_stage_t stage;

    char url[384];
    char host[256];
    char path[512];
    uint16_t port;
    uint32_t ip;

    int dns_handle;

    netd_tcp_conn_t* tcp;
    uint32_t tcp_start_ms;

    char req_buf[1024];
    uint32_t req_len;
    uint32_t req_off;

    uint8_t hdr_buf[2048];
    uint32_t hdr_w;
    uint32_t body_off;

    uint32_t http_status;
    uint32_t content_length;
    int chunked;

    uint32_t pf_r;
    uint32_t pf_w;

    netd_http_body_mode_t body_mode;
    uint32_t body_remaining;

    char chunk_line[64];
    uint32_t chunk_line_len;
    uint32_t chunk_remaining;
};

static void netd_http_job_send_begin_once(netd_http_job_t* j, uint32_t status, uint32_t http_status, uint32_t content_length) {
    if (!j || j->fd_out < 0 || j->begin_sent) {
        return;
    }
    netd_http_send_begin(j->fd_out, j->seq, status, http_status, content_length);
    j->begin_sent = 1;
}

static netd_http_job_t g_jobs[NETD_HTTP_MAX_JOBS];

static void netd_http_job_reset(netd_http_job_t* j) {
    if (!j) {
        return;
    }
    memset(j, 0, sizeof(*j));
    j->fd_out = -1;
    j->dns_handle = -1;
    j->stage = NETD_HTTP_JOB_DONE;
}

static netd_http_job_t* netd_http_job_alloc(void) {
    for (int i = 0; i < NETD_HTTP_MAX_JOBS; i++) {
        if (!g_jobs[i].active) {
            netd_http_job_reset(&g_jobs[i]);
            g_jobs[i].active = 1;
            return &g_jobs[i];
        }
    }
    return 0;
}

static void netd_http_job_finish(netd_ctx_t* ctx, netd_http_job_t* j, uint32_t status) {
    if (!j) {
        return;
    }

    if (ctx && j->dns_handle >= 0) {
        netd_dns_query_cancel(ctx, j->dns_handle);
        j->dns_handle = -1;
    }

    if (ctx && j->tcp) {
        uint32_t st = 0;
        (void)netd_tcp_close_start(ctx, j->tcp, &st);
        (void)netd_tcp_close_poll(ctx, j->tcp, uptime_ms(), 0, &st);
        j->tcp = 0;
    }

    if (j->fd_out >= 0) {
        netd_http_job_send_begin_once(j, status, 0, 0);
        netd_http_send_end(j->fd_out, j->seq, status);
    }

    netd_http_job_reset(j);
}

static int netd_http_job_deadline_expired(const netd_http_job_t* j) {
    if (!j) {
        return 1;
    }
    uint32_t now_ms = uptime_ms();
    return (now_ms - j->stage_start_ms) >= j->timeout_ms;
}

static uint32_t netd_http_job_read_bytes(netd_http_job_t* j, netd_tcp_conn_t* tcp, uint8_t* out, uint32_t cap) {
    if (!j || !out || cap == 0) {
        return 0;
    }

    uint32_t got = 0;
    if (j->pf_r < j->pf_w) {
        uint32_t avail = j->pf_w - j->pf_r;
        if (cap > avail) {
            cap = avail;
        }
        memcpy(out, j->hdr_buf + j->body_off + j->pf_r, cap);
        j->pf_r += cap;
        return cap;
    }

    if (!tcp) {
        return 0;
    }

    got = netd_tcp_recv_nowait(tcp, out, cap);
    return got;
}

static int netd_http_job_readline(netd_http_job_t* j, netd_tcp_conn_t* tcp, char* out, uint32_t cap) {
    if (!j || !out || cap < 2u) {
        return 0;
    }

    for (;;) {
        if (j->chunk_line_len + 1u >= (uint32_t)sizeof(j->chunk_line)) {
            return 0;
        }

        uint8_t b = 0;
        uint32_t got = netd_http_job_read_bytes(j, tcp, &b, 1u);
        if (got == 0) {
            return 0;
        }

        j->chunk_line[j->chunk_line_len++] = (char)b;
        j->chunk_line[j->chunk_line_len] = '\0';

        if (j->chunk_line_len >= 2u) {
            uint32_t n = j->chunk_line_len;
            if (j->chunk_line[n - 2u] == '\r' && j->chunk_line[n - 1u] == '\n') {
                j->chunk_line[n - 2u] = '\0';
                uint32_t to_copy = n - 1u;
                if (to_copy > 0 && j->chunk_line[to_copy - 1u] == '\r') {
                    to_copy--;
                }
                if (to_copy + 1u > cap) {
                    return 0;
                }
                memcpy(out, j->chunk_line, to_copy + 1u);
                j->chunk_line_len = 0;
                return 1;
            }
        }
    }
}

static int netd_http_job_start_request(netd_http_job_t* j) {
    if (!j) {
        return 0;
    }

    int rn = snprintf(
        j->req_buf,
        sizeof(j->req_buf),
        "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: yulaos-wget/1\r\nConnection: close\r\n\r\n",
        j->path,
        j->host
    );
    if (rn <= 0 || (uint32_t)rn >= (uint32_t)sizeof(j->req_buf)) {
        return 0;
    }
    j->req_len = (uint32_t)rn;
    j->req_off = 0;
    return 1;
}

static void netd_http_job_tick(netd_ctx_t* ctx, netd_http_job_t* j) {
    if (!ctx || !j || !j->active) {
        return;
    }

    if (j->stage != NETD_HTTP_JOB_DONE && netd_http_job_deadline_expired(j)) {
        netd_http_job_finish(ctx, j, NET_STATUS_TIMEOUT);
        return;
    }

    if (j->stage == NETD_HTTP_JOB_PARSE_URL) {
        uint16_t port = 80u;
        int is_https = 0;
        if (!netd_http_parse_url(j->url, j->host, (uint32_t)sizeof(j->host), &port, j->path, (uint32_t)sizeof(j->path), &is_https)) {
            netd_http_send_stage(j->fd_out, j->seq, NET_HTTP_GET_STAGE_PARSE_URL, NET_STATUS_UNSUPPORTED, 0, NET_HTTP_GET_STAGE_F_BEGIN | NET_HTTP_GET_STAGE_F_END);
            netd_http_job_send_begin_once(j, NET_STATUS_UNSUPPORTED, 0, 0);
            netd_http_job_finish(ctx, j, NET_STATUS_UNSUPPORTED);
            return;
        }

        if (is_https) {
            netd_http_send_stage(j->fd_out, j->seq, NET_HTTP_GET_STAGE_PARSE_URL, NET_STATUS_UNSUPPORTED, 1u, NET_HTTP_GET_STAGE_F_BEGIN | NET_HTTP_GET_STAGE_F_END);
            netd_http_job_send_begin_once(j, NET_STATUS_UNSUPPORTED, 0, 0);
            netd_http_job_finish(ctx, j, NET_STATUS_UNSUPPORTED);
            return;
        }

        j->port = port;
        netd_http_send_stage(j->fd_out, j->seq, NET_HTTP_GET_STAGE_PARSE_URL, NET_STATUS_OK, 0, NET_HTTP_GET_STAGE_F_BEGIN | NET_HTTP_GET_STAGE_F_END);

        netd_http_send_stage(j->fd_out, j->seq, NET_HTTP_GET_STAGE_DNS, NET_STATUS_OK, 0, NET_HTTP_GET_STAGE_F_BEGIN);
        j->dns_handle = netd_dns_query_start(ctx, j->host, j->timeout_ms);
        if (j->dns_handle < 0) {
            netd_http_send_stage(j->fd_out, j->seq, NET_HTTP_GET_STAGE_DNS, NET_STATUS_ERROR, 0, NET_HTTP_GET_STAGE_F_END);
            netd_http_job_send_begin_once(j, NET_STATUS_ERROR, 0, 0);
            netd_http_job_finish(ctx, j, NET_STATUS_ERROR);
            return;
        }

        j->stage = NETD_HTTP_JOB_DNS;
        j->stage_start_ms = uptime_ms();
        return;
    }

    if (j->stage == NETD_HTTP_JOB_DNS) {
        uint32_t ip = 0;
        uint32_t st = 0;
        int done = netd_dns_query_poll(ctx, j->dns_handle, &ip, &st);
        if (!done) {
            return;
        }

        j->dns_handle = -1;
        if (st != NET_STATUS_OK || ip == 0) {
            netd_http_send_stage(j->fd_out, j->seq, NET_HTTP_GET_STAGE_DNS, (st != 0) ? st : NET_STATUS_ERROR, 0, NET_HTTP_GET_STAGE_F_END);
            netd_http_job_send_begin_once(j, (st != 0) ? st : NET_STATUS_ERROR, 0, 0);
            netd_http_job_finish(ctx, j, (st != 0) ? st : NET_STATUS_ERROR);
            return;
        }

        j->ip = ip;
        netd_http_send_stage(j->fd_out, j->seq, NET_HTTP_GET_STAGE_DNS, NET_STATUS_OK, ip, NET_HTTP_GET_STAGE_F_END);

        netd_http_send_stage(j->fd_out, j->seq, NET_HTTP_GET_STAGE_CONNECT, NET_STATUS_OK, j->port, NET_HTTP_GET_STAGE_F_BEGIN);
        uint32_t conn_status = NET_STATUS_ERROR;
        j->tcp = netd_tcp_open_start(ctx, j->ip, j->port, &conn_status);
        if (!j->tcp) {
            uint32_t s = (conn_status != 0) ? conn_status : NET_STATUS_ERROR;
            netd_http_send_stage(j->fd_out, j->seq, NET_HTTP_GET_STAGE_CONNECT, s, j->port, NET_HTTP_GET_STAGE_F_END);
            netd_http_job_send_begin_once(j, s, 0, 0);
            netd_http_job_finish(ctx, j, s);
            return;
        }
        j->tcp_start_ms = uptime_ms();
        j->stage = NETD_HTTP_JOB_CONNECT;
        j->stage_start_ms = uptime_ms();
        return;
    }

    if (j->stage == NETD_HTTP_JOB_CONNECT) {
        uint32_t st = 0;
        int done = netd_tcp_open_poll(ctx, j->tcp, j->tcp_start_ms, j->timeout_ms, &st);
        if (!done) {
            return;
        }
        if (st != NET_STATUS_OK) {
            netd_http_send_stage(j->fd_out, j->seq, NET_HTTP_GET_STAGE_CONNECT, st, j->port, NET_HTTP_GET_STAGE_F_END);
            netd_http_job_send_begin_once(j, st, 0, 0);
            netd_http_job_finish(ctx, j, st);
            return;
        }

        netd_http_send_stage(j->fd_out, j->seq, NET_HTTP_GET_STAGE_CONNECT, NET_STATUS_OK, j->port, NET_HTTP_GET_STAGE_F_END);

        if (!netd_http_job_start_request(j)) {
            netd_http_job_send_begin_once(j, NET_STATUS_ERROR, 0, 0);
            netd_http_job_finish(ctx, j, NET_STATUS_ERROR);
            return;
        }

        netd_http_send_stage(j->fd_out, j->seq, NET_HTTP_GET_STAGE_SEND_REQUEST, NET_STATUS_OK, 0, NET_HTTP_GET_STAGE_F_BEGIN);
        j->stage = NETD_HTTP_JOB_SEND_REQ;
        j->stage_start_ms = uptime_ms();
        return;
    }

    if (j->stage == NETD_HTTP_JOB_SEND_REQ) {
        uint32_t st = 0;
        int done = netd_tcp_send_poll(ctx, j->tcp, j->req_buf, j->req_len, &j->req_off, j->stage_start_ms, j->timeout_ms, &st);
        if (!done) {
            return;
        }
        if (st != NET_STATUS_OK) {
            netd_http_send_stage(j->fd_out, j->seq, NET_HTTP_GET_STAGE_SEND_REQUEST, st, 0, NET_HTTP_GET_STAGE_F_END);
            netd_http_job_send_begin_once(j, st, 0, 0);
            netd_http_job_finish(ctx, j, st);
            return;
        }

        netd_http_send_stage(j->fd_out, j->seq, NET_HTTP_GET_STAGE_SEND_REQUEST, NET_STATUS_OK, 0, NET_HTTP_GET_STAGE_F_END);

        netd_http_send_stage(j->fd_out, j->seq, NET_HTTP_GET_STAGE_RECV_HEADERS, NET_STATUS_OK, 0, NET_HTTP_GET_STAGE_F_BEGIN);
        j->hdr_w = 0;
        j->body_off = 0;
        j->stage = NETD_HTTP_JOB_RECV_HDR;
        j->stage_start_ms = uptime_ms();
        return;
    }

    if (j->stage == NETD_HTTP_JOB_RECV_HDR) {
        if (j->hdr_w >= (uint32_t)sizeof(j->hdr_buf)) {
            netd_http_send_stage(j->fd_out, j->seq, NET_HTTP_GET_STAGE_RECV_HEADERS, NET_STATUS_ERROR, 0, NET_HTTP_GET_STAGE_F_END);
            netd_http_job_send_begin_once(j, NET_STATUS_ERROR, 0, 0);
            netd_http_job_finish(ctx, j, NET_STATUS_ERROR);
            return;
        }

        uint32_t cap = (uint32_t)sizeof(j->hdr_buf) - j->hdr_w;
        uint32_t got = netd_tcp_recv_nowait(j->tcp, j->hdr_buf + j->hdr_w, cap);
        if (got == 0) {
            return;
        }

        j->hdr_w += got;
        if (!netd_http_find_crlfcrlf(j->hdr_buf, j->hdr_w, &j->body_off)) {
            return;
        }

        if (!netd_http_parse_status_line(j->hdr_buf, j->body_off, &j->http_status)) {
            netd_http_send_stage(j->fd_out, j->seq, NET_HTTP_GET_STAGE_RECV_HEADERS, NET_STATUS_ERROR, 0, NET_HTTP_GET_STAGE_F_END);
            netd_http_job_send_begin_once(j, NET_STATUS_ERROR, 0, 0);
            netd_http_job_finish(ctx, j, NET_STATUS_ERROR);
            return;
        }

        j->content_length = 0;
        j->chunked = 0;
        char location[384];
        if (!netd_http_parse_headers(j->hdr_buf, j->body_off, &j->content_length, &j->chunked, location, (uint32_t)sizeof(location))) {
            netd_http_send_stage(j->fd_out, j->seq, NET_HTTP_GET_STAGE_RECV_HEADERS, NET_STATUS_ERROR, j->http_status, NET_HTTP_GET_STAGE_F_END);
            netd_http_job_send_begin_once(j, NET_STATUS_ERROR, j->http_status, 0);
            netd_http_job_finish(ctx, j, NET_STATUS_ERROR);
            return;
        }

        netd_http_send_stage(j->fd_out, j->seq, NET_HTTP_GET_STAGE_RECV_HEADERS, NET_STATUS_OK, j->http_status, NET_HTTP_GET_STAGE_F_END);

        if (j->http_status < 200u || j->http_status >= 300u) {
            netd_http_job_send_begin_once(j, NET_STATUS_ERROR, j->http_status, 0);
            netd_http_job_finish(ctx, j, NET_STATUS_ERROR);
            return;
        }

        netd_http_job_send_begin_once(j, NET_STATUS_OK, j->http_status, j->chunked ? 0u : j->content_length);
        netd_http_send_stage(j->fd_out, j->seq, NET_HTTP_GET_STAGE_RECV_BODY, NET_STATUS_OK, (uint32_t)j->chunked, NET_HTTP_GET_STAGE_F_BEGIN);

        j->pf_r = 0;
        j->pf_w = (j->hdr_w > j->body_off) ? (j->hdr_w - j->body_off) : 0;

        if (j->chunked) {
            j->body_mode = NETD_HTTP_BODY_MODE_CHUNK_SIZE;
        } else if (j->content_length > 0) {
            j->body_mode = NETD_HTTP_BODY_MODE_CL;
            j->body_remaining = j->content_length;
        } else {
            j->body_mode = NETD_HTTP_BODY_MODE_UNTIL_CLOSE;
            j->body_remaining = 0xFFFFFFFFu;
        }

        j->chunk_line_len = 0;
        j->chunk_remaining = 0;

        j->stage = NETD_HTTP_JOB_RECV_BODY;
        j->stage_start_ms = uptime_ms();
        return;
    }

    if (j->stage == NETD_HTTP_JOB_RECV_BODY) {
        uint8_t buf[512];

        if (j->body_mode == NETD_HTTP_BODY_MODE_CL || j->body_mode == NETD_HTTP_BODY_MODE_UNTIL_CLOSE) {
            uint32_t cap = (uint32_t)sizeof(buf);
            if (j->body_mode == NETD_HTTP_BODY_MODE_CL && cap > j->body_remaining) {
                cap = j->body_remaining;
            }
            if (cap == 0) {
                netd_http_send_stage(j->fd_out, j->seq, NET_HTTP_GET_STAGE_RECV_BODY, NET_STATUS_OK, (uint32_t)j->chunked, NET_HTTP_GET_STAGE_F_END);
                netd_http_job_finish(ctx, j, NET_STATUS_OK);
                return;
            }

            uint32_t got = netd_http_job_read_bytes(j, j->tcp, buf, cap);
            if (got == 0) {
                if (j->body_mode == NETD_HTTP_BODY_MODE_UNTIL_CLOSE && j->tcp && j->tcp->remote_closed) {
                    netd_http_send_stage(j->fd_out, j->seq, NET_HTTP_GET_STAGE_RECV_BODY, NET_STATUS_OK, (uint32_t)j->chunked, NET_HTTP_GET_STAGE_F_END);
                    netd_http_job_finish(ctx, j, NET_STATUS_OK);
                }
                return;
            }

            netd_http_send_data(j->fd_out, j->seq, buf, got);

            if (j->body_mode == NETD_HTTP_BODY_MODE_CL) {
                if (got >= j->body_remaining) {
                    j->body_remaining = 0;
                } else {
                    j->body_remaining -= got;
                }
            }

            return;
        }

        if (j->body_mode == NETD_HTTP_BODY_MODE_CHUNK_SIZE) {
            char line[64];
            if (!netd_http_job_readline(j, j->tcp, line, (uint32_t)sizeof(line))) {
                return;
            }

            uint32_t sz = 0;
            if (!netd_http_parse_hex_u32(line, &sz)) {
                netd_http_send_stage(j->fd_out, j->seq, NET_HTTP_GET_STAGE_RECV_BODY, NET_STATUS_ERROR, 1u, NET_HTTP_GET_STAGE_F_END);
                netd_http_job_finish(ctx, j, NET_STATUS_ERROR);
                return;
            }

            if (sz == 0) {
                j->body_mode = NETD_HTTP_BODY_MODE_CHUNK_TRAILERS;
                return;
            }

            j->chunk_remaining = sz;
            j->body_mode = NETD_HTTP_BODY_MODE_CHUNK_DATA;
            return;
        }

        if (j->body_mode == NETD_HTTP_BODY_MODE_CHUNK_DATA) {
            uint32_t cap = (uint32_t)sizeof(buf);
            if (cap > j->chunk_remaining) {
                cap = j->chunk_remaining;
            }
            if (cap == 0) {
                j->body_mode = NETD_HTTP_BODY_MODE_CHUNK_CRLF;
                return;
            }

            uint32_t got = netd_http_job_read_bytes(j, j->tcp, buf, cap);
            if (got == 0) {
                return;
            }
            netd_http_send_data(j->fd_out, j->seq, buf, got);
            if (got >= j->chunk_remaining) {
                j->chunk_remaining = 0;
            } else {
                j->chunk_remaining -= got;
            }

            if (j->chunk_remaining == 0) {
                j->body_mode = NETD_HTTP_BODY_MODE_CHUNK_CRLF;
            }
            return;
        }

        if (j->body_mode == NETD_HTTP_BODY_MODE_CHUNK_CRLF) {
            uint8_t crlf[2];
            uint32_t got = netd_http_job_read_bytes(j, j->tcp, crlf, 2u);
            if (got < 2u) {
                if (got == 1u) {
                    j->pf_r -= 1u;
                }
                return;
            }
            if (crlf[0] != '\r' || crlf[1] != '\n') {
                netd_http_send_stage(j->fd_out, j->seq, NET_HTTP_GET_STAGE_RECV_BODY, NET_STATUS_ERROR, 1u, NET_HTTP_GET_STAGE_F_END);
                netd_http_job_finish(ctx, j, NET_STATUS_ERROR);
                return;
            }
            j->body_mode = NETD_HTTP_BODY_MODE_CHUNK_SIZE;
            return;
        }

        if (j->body_mode == NETD_HTTP_BODY_MODE_CHUNK_TRAILERS) {
            char line[64];
            if (!netd_http_job_readline(j, j->tcp, line, (uint32_t)sizeof(line))) {
                return;
            }
            if (line[0] != '\0') {
                return;
            }

            netd_http_send_stage(j->fd_out, j->seq, NET_HTTP_GET_STAGE_RECV_BODY, NET_STATUS_OK, 1u, NET_HTTP_GET_STAGE_F_END);
            netd_http_job_finish(ctx, j, NET_STATUS_OK);
            return;
        }

        netd_http_send_stage(j->fd_out, j->seq, NET_HTTP_GET_STAGE_RECV_BODY, NET_STATUS_ERROR, 0, NET_HTTP_GET_STAGE_F_END);
        netd_http_job_finish(ctx, j, NET_STATUS_ERROR);
        return;
    }
}

int netd_http_get_start(netd_ctx_t* ctx, int fd_out, uint32_t seq, const net_http_get_req_t* req) {
    if (!ctx || fd_out < 0 || !req) {
        return -1;
    }

    uint32_t timeout_ms = req->timeout_ms;
    if (timeout_ms == 0) {
        timeout_ms = 5000u;
    }

    netd_http_job_t* j = netd_http_job_alloc();
    if (!j) {
        netd_http_send_begin(fd_out, seq, NET_STATUS_ERROR, 0, 0);
        netd_http_send_end(fd_out, seq, NET_STATUS_ERROR);
        return -1;
    }

    j->fd_out = fd_out;
    j->seq = seq;
    j->timeout_ms = timeout_ms;
    j->start_ms = uptime_ms();
    j->stage_start_ms = j->start_ms;
    j->stage = NETD_HTTP_JOB_PARSE_URL;

    strncpy(j->url, req->url, (uint32_t)sizeof(j->url) - 1u);
    j->url[(uint32_t)sizeof(j->url) - 1u] = '\0';

    netd_http_job_tick(ctx, j);
    return 0;
}

void netd_http_tick(netd_ctx_t* ctx) {
    if (!ctx) {
        return;
    }
    for (int i = 0; i < NETD_HTTP_MAX_JOBS; i++) {
        if (!g_jobs[i].active) {
            continue;
        }
        netd_http_job_tick(ctx, &g_jobs[i]);
    }
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
    netd_tcp_conn_t* tcp;
} netd_http_io_t;

static int netd_http_io_send(netd_ctx_t* ctx, netd_http_io_t* io, const void* data, uint32_t len, uint32_t timeout_ms) {
    if (!ctx || !io) {
        return 0;
    }

    if (io->use_tls) {
        return netd_tls_send(ctx, &io->tls, data, len, timeout_ms);
    }

    if (!io->tcp) {
        return 0;
    }
    return netd_tcp_send(ctx, io->tcp, data, len, timeout_ms);
}

static int netd_http_io_recv(netd_ctx_t* ctx, netd_http_io_t* io, void* out, uint32_t cap, uint32_t timeout_ms, uint32_t* out_n) {
    if (!ctx || !io) {
        return 0;
    }

    if (io->use_tls) {
        return netd_tls_recv(ctx, &io->tls, out, cap, timeout_ms, out_n);
    }

    if (!io->tcp) {
        return 0;
    }
    return netd_tcp_recv(ctx, io->tcp, out, cap, timeout_ms, out_n);
}

static int netd_http_io_close(netd_ctx_t* ctx, netd_http_io_t* io, uint32_t timeout_ms) {
    if (!ctx || !io) {
        return 0;
    }

    if (io->use_tls) {
        return netd_tls_close(ctx, &io->tls, timeout_ms);
    }

    if (!io->tcp) {
        return 0;
    }
    return netd_tcp_close(ctx, io->tcp, timeout_ms);
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
    io.tcp = 0;

    netd_http_send_stage(fd_out, seq, NET_HTTP_GET_STAGE_CONNECT, NET_STATUS_OK, port, NET_HTTP_GET_STAGE_F_BEGIN);
    uint32_t conn_status = NET_STATUS_ERROR;
    io.tcp = netd_tcp_open(ctx, ip, port, timeout_ms, &conn_status);
    if (!io.tcp) {
        uint32_t st = (conn_status != 0) ? conn_status : NET_STATUS_ERROR;
        netd_http_send_stage(fd_out, seq, NET_HTTP_GET_STAGE_CONNECT, st, port, NET_HTTP_GET_STAGE_F_END);
        netd_http_send_begin(fd_out, seq, st, 0, 0);
        netd_http_send_end(fd_out, seq, st);
        return st;
    }
    netd_http_send_stage(fd_out, seq, NET_HTTP_GET_STAGE_CONNECT, NET_STATUS_OK, port, NET_HTTP_GET_STAGE_F_END);

    if (io.use_tls) {
        netd_http_send_stage(fd_out, seq, NET_HTTP_GET_STAGE_TLS_HANDSHAKE, NET_STATUS_OK, 0, NET_HTTP_GET_STAGE_F_BEGIN);
        if (!netd_tls_handshake(ctx, &io.tls, io.tcp, host, timeout_ms)) {
            (void)netd_tcp_close(ctx, io.tcp, timeout_ms);
            io.tcp = 0;
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
