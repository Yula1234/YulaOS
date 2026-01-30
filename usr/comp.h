// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#ifndef YOS_COMP_H
#define YOS_COMP_H

#include <yula.h>
#include <comp_ipc.h>

#define COMP_RX_CAP 2048u
#define COMP_RX_MASK (COMP_RX_CAP - 1u)

#define COMP_PENDING_MAX 8u

typedef struct {
    uint8_t buf[COMP_RX_CAP];
    uint32_t r;
    uint32_t w;
} comp_rx_ring_t;

static inline uint32_t comp_rx_count(const comp_rx_ring_t* q) {
    return q->w - q->r;
}

static inline void comp_rx_drop(comp_rx_ring_t* q, uint32_t n) {
    uint32_t c = comp_rx_count(q);
    if (n > c) n = c;
    q->r += n;
}

static inline void comp_rx_peek(const comp_rx_ring_t* q, uint32_t off, void* dst, uint32_t n) {
    uint8_t* out = (uint8_t*)dst;
    uint32_t ri = (q->r + off) & COMP_RX_MASK;
    uint32_t first = COMP_RX_CAP - ri;
    if (first > n) first = n;
    memcpy(out, &q->buf[ri], first);
    if (n > first) memcpy(out + first, &q->buf[0], n - first);
}

static inline void comp_rx_push(comp_rx_ring_t* q, const uint8_t* src, uint32_t n) {
    if (!q || !src || n == 0) return;

    uint32_t count = comp_rx_count(q);
    if (n > COMP_RX_CAP) {
        src += (n - COMP_RX_CAP);
        n = COMP_RX_CAP;
        q->r = 0;
        q->w = 0;
        count = 0;
    }
    if (count + n > COMP_RX_CAP) {
        uint32_t drop = (count + n) - COMP_RX_CAP;
        q->r += drop;
    }

    uint32_t wi = q->w & COMP_RX_MASK;
    uint32_t first = COMP_RX_CAP - wi;
    if (first > n) first = n;
    memcpy(&q->buf[wi], src, first);
    if (n > first) memcpy(&q->buf[0], src + first, n - first);
    q->w += n;
}

typedef struct {
    int connected;
    int fd_c2s_w;
    int fd_s2c_r;
    uint32_t seq;
    comp_rx_ring_t rx;

    int input_ring_shm_fd;
    uint32_t input_ring_size_bytes;
    char input_ring_shm_name[32];
    comp_input_ring_t* input_ring;
    int input_ring_enabled;

    uint32_t pending_r;
    uint32_t pending_w;
    struct {
        comp_ipc_hdr_t hdr;
        uint8_t payload[COMP_IPC_MAX_PAYLOAD];
    } pending[COMP_PENDING_MAX];
} comp_conn_t;

static inline void comp_input_ring_close(comp_conn_t* c) {
    if (!c) return;
    if (c->input_ring) {
        munmap((void*)c->input_ring, c->input_ring_size_bytes);
        c->input_ring = 0;
    }
    if (c->input_ring_shm_fd >= 0) {
        close(c->input_ring_shm_fd);
        c->input_ring_shm_fd = -1;
    }
    c->input_ring_size_bytes = 0;
    c->input_ring_shm_name[0] = '\0';
    c->input_ring_enabled = 0;
}

static inline int comp_input_ring_try_pop(comp_conn_t* c, comp_ipc_input_t* out_ev) {
    if (!c || !out_ev) return 0;
    if (!c->input_ring_enabled || !c->input_ring) return 0;
    comp_input_ring_t* ring = c->input_ring;
    if ((ring->flags & COMP_INPUT_RING_FLAG_READY) == 0) return 0;

    const uint32_t r = ring->r;
    const uint32_t w = ring->w;
    if (r == w) return 0;

    const uint32_t ri = r & ring->mask;
    __sync_synchronize();
    *out_ev = ring->events[ri];
    __sync_synchronize();
    ring->r = r + 1u;
    __sync_synchronize();

    if (ring->flags & COMP_INPUT_RING_FLAG_WAIT_W) {
        (void)__sync_fetch_and_and(&ring->flags, ~COMP_INPUT_RING_FLAG_WAIT_W);
        futex_wake(&ring->r, 1u);
    }
    return 1;
}

static inline void comp_wait_events(comp_conn_t* c, uint32_t fallback_us) {
    if (!c || !c->connected) {
        if (fallback_us) usleep(fallback_us);
        return;
    }

    if (c->input_ring_enabled && c->input_ring && (c->input_ring->flags & COMP_INPUT_RING_FLAG_READY)) {
        comp_input_ring_t* ring = c->input_ring;
        if (fallback_us) {
            const uint32_t r = ring->r;
            const uint32_t w = ring->w;
            if (r != w) return;

            (void)__sync_fetch_and_or(&ring->flags, COMP_INPUT_RING_FLAG_WAIT_R);
            __sync_synchronize();

            if (ring->r != ring->w) {
                (void)__sync_fetch_and_and(&ring->flags, ~COMP_INPUT_RING_FLAG_WAIT_R);
                return;
            }

            usleep(fallback_us);
            (void)__sync_fetch_and_and(&ring->flags, ~COMP_INPUT_RING_FLAG_WAIT_R);
            return;
        }
        for (;;) {
            uint32_t r = ring->r;
            uint32_t w = ring->w;
            if (r != w) return;

            (void)__sync_fetch_and_or(&ring->flags, COMP_INPUT_RING_FLAG_WAIT_R);
            __sync_synchronize();

            r = ring->r;
            w = ring->w;
            if (r != w) {
                (void)__sync_fetch_and_and(&ring->flags, ~COMP_INPUT_RING_FLAG_WAIT_R);
                return;
            }

            (void)futex_wait(&ring->w, w);
            (void)__sync_fetch_and_and(&ring->flags, ~COMP_INPUT_RING_FLAG_WAIT_R);
        }
    }

    if (fallback_us) usleep(fallback_us);
}

static inline uint32_t comp_pending_count(const comp_conn_t* c) {
    if (!c) return 0;
    return c->pending_w - c->pending_r;
}

static inline int comp_pending_push(comp_conn_t* c, const comp_ipc_hdr_t* hdr, const void* payload) {
    if (!c || !hdr) return -1;
    if (hdr->len > COMP_IPC_MAX_PAYLOAD) return -1;
    uint32_t count = comp_pending_count(c);
    if (count >= COMP_PENDING_MAX) return -1;
    uint32_t wi = c->pending_w % COMP_PENDING_MAX;
    c->pending[wi].hdr = *hdr;
    if (hdr->len) {
        memcpy(c->pending[wi].payload, payload, hdr->len);
    }
    c->pending_w++;
    return 0;
}

static inline int comp_pending_pop(comp_conn_t* c, comp_ipc_hdr_t* out_hdr, void* out_payload, uint32_t payload_cap) {
    if (!c) return 0;
    uint32_t count = comp_pending_count(c);
    if (count == 0) return 0;
    uint32_t ri = c->pending_r % COMP_PENDING_MAX;
    const comp_ipc_hdr_t hdr = c->pending[ri].hdr;
    if (hdr.len > payload_cap || (!out_payload && hdr.len)) return -1;
    if (out_hdr) *out_hdr = hdr;
    if (hdr.len) memcpy(out_payload, c->pending[ri].payload, hdr.len);
    c->pending_r++;
    return 1;
}

static inline int comp_is_ack_for(const comp_ipc_hdr_t* hdr, const void* payload, uint32_t req_type, uint32_t surface_id) {
    if (!hdr || !payload) return 0;
    if (hdr->type != (uint16_t)COMP_IPC_MSG_ACK) return 0;
    if (hdr->len != (uint32_t)sizeof(comp_ipc_ack_t)) return 0;
    const comp_ipc_ack_t* a = (const comp_ipc_ack_t*)payload;
    if (a->req_type != (uint16_t)req_type) return 0;
    if (surface_id && a->surface_id != surface_id) return 0;
    return 1;
}

static inline int comp_is_error_for(const comp_ipc_hdr_t* hdr, const void* payload, uint32_t req_type, uint32_t surface_id, uint16_t* out_code) {
    if (!hdr || !payload) return 0;
    if (hdr->type != (uint16_t)COMP_IPC_MSG_ERROR) return 0;
    if (hdr->len != (uint32_t)sizeof(comp_ipc_error_t)) return 0;
    const comp_ipc_error_t* e = (const comp_ipc_error_t*)payload;
    if (e->req_type != (uint16_t)req_type) return 0;
    if (surface_id && e->surface_id != surface_id) return 0;
    if (out_code) *out_code = e->code;
    return 1;
}

static inline int comp_pending_take_for_seq(comp_conn_t* c,
                                            uint32_t want_seq,
                                            comp_ipc_hdr_t* out_hdr,
                                            void* out_payload,
                                            uint32_t payload_cap) {
    if (!c) return 0;
    uint32_t count = comp_pending_count(c);
    for (uint32_t i = 0; i < count; i++) {
        comp_ipc_hdr_t hdr;
        uint8_t payload[COMP_IPC_MAX_PAYLOAD];
        int pr = comp_pending_pop(c, &hdr, payload, (uint32_t)sizeof(payload));
        if (pr <= 0) return 0;
        if (hdr.seq == want_seq) {
            if (hdr.len > payload_cap || (!out_payload && hdr.len)) return -1;
            if (out_hdr) *out_hdr = hdr;
            if (hdr.len) memcpy(out_payload, payload, hdr.len);
            return 1;
        }
        if (comp_pending_push(c, &hdr, payload) != 0) return -1;
    }
    return 0;
}

static inline int comp_try_recv_raw(comp_conn_t* c, comp_ipc_hdr_t* out_hdr, void* out_payload, uint32_t payload_cap) {
    if (!c || !c->connected || c->fd_s2c_r < 0) return -1;

    int saw_eof = 0;

    for (;;) {
        const uint32_t cap = (uint32_t)sizeof(c->rx.buf);
        uint32_t count = comp_rx_count(&c->rx);
        uint32_t space = (count < cap) ? (cap - count) : 0u;
        const uint32_t reserve = (uint32_t)sizeof(comp_ipc_hdr_t) + (uint32_t)COMP_IPC_MAX_PAYLOAD;
        if (space <= reserve) break;
        space -= reserve;

        uint8_t tmp[512];
        uint32_t want = space;
        if (want > (uint32_t)sizeof(tmp)) want = (uint32_t)sizeof(tmp);

        int rn = pipe_try_read(c->fd_s2c_r, tmp, want);
        if (rn < 0) {
            saw_eof = 1;
            break;
        }
        if (rn == 0) break;
        comp_rx_push(&c->rx, tmp, (uint32_t)rn);
    }

    for (;;) {
        uint32_t avail = comp_rx_count(&c->rx);
        if (avail < 4u) return saw_eof ? -1 : 0;

        uint32_t magic;
        comp_rx_peek(&c->rx, 0, &magic, 4u);
        if (magic != COMP_IPC_MAGIC) {
            comp_rx_drop(&c->rx, 1u);
            continue;
        }

        if (avail < (uint32_t)sizeof(comp_ipc_hdr_t)) return saw_eof ? -1 : 0;

        comp_ipc_hdr_t hdr;
        comp_rx_peek(&c->rx, 0, &hdr, (uint32_t)sizeof(hdr));

        if (hdr.version != COMP_IPC_VERSION || hdr.len > COMP_IPC_MAX_PAYLOAD) {
            comp_rx_drop(&c->rx, 1u);
            continue;
        }

        uint32_t frame_len = (uint32_t)sizeof(hdr) + hdr.len;
        if (avail < frame_len) return saw_eof ? -1 : 0;

        comp_rx_drop(&c->rx, (uint32_t)sizeof(hdr));

        if (hdr.len) {
            if (hdr.len > payload_cap || !out_payload) {
                comp_rx_drop(&c->rx, hdr.len);
                return -1;
            }
            comp_rx_peek(&c->rx, 0, out_payload, hdr.len);
            comp_rx_drop(&c->rx, hdr.len);
        }

        if (hdr.type == (uint16_t)COMP_IPC_MSG_INPUT_RING_NAME && hdr.len == (uint32_t)sizeof(comp_ipc_input_ring_name_t)) {
            comp_ipc_input_ring_name_t msg;
            memcpy(&msg, out_payload, sizeof(msg));
            msg.shm_name[sizeof(msg.shm_name) - 1u] = '\0';

            if (!c->input_ring && msg.size_bytes >= (uint32_t)sizeof(comp_input_ring_t) && msg.shm_name[0]) {
                int fd = shm_open_named(msg.shm_name);
                if (fd >= 0) {
                    comp_input_ring_t* ring = (comp_input_ring_t*)mmap(fd, msg.size_bytes, MAP_SHARED);
                    if (ring && ring->magic == COMP_INPUT_RING_MAGIC && ring->version == COMP_INPUT_RING_VERSION) {
                        c->input_ring_shm_fd = fd;
                        c->input_ring_size_bytes = msg.size_bytes;
                        memcpy(c->input_ring_shm_name, msg.shm_name, sizeof(c->input_ring_shm_name));
                        c->input_ring = ring;
                        c->input_ring_enabled = 1;
                        if (c->fd_c2s_w >= 0) {
                            (void)comp_ipc_send(c->fd_c2s_w, (uint16_t)COMP_IPC_MSG_INPUT_RING_ACK, c->seq++, 0, 0);
                        }
                    } else {
                        if (ring) munmap((void*)ring, msg.size_bytes);
                        close(fd);
                    }
                }
            }
            continue;
        }

        if (out_hdr) *out_hdr = hdr;
        return 1;
    }
}

static inline int comp_wait_reply(comp_conn_t* c,
                                  uint32_t want_seq,
                                  comp_ipc_hdr_t* out_hdr,
                                  void* out_payload,
                                  uint32_t payload_cap,
                                  uint32_t max_iters) {
    if (!c || !c->connected) return -1;
    if (max_iters == 0) max_iters = 1;

    for (uint32_t i = 0; i < max_iters; i++) {
        int pr = comp_pending_take_for_seq(c, want_seq, out_hdr, out_payload, payload_cap);
        if (pr != 0) return pr;

        comp_ipc_hdr_t hdr;
        uint8_t payload[COMP_IPC_MAX_PAYLOAD];
        int r = comp_try_recv_raw(c, &hdr, payload, (uint32_t)sizeof(payload));
        if (r < 0) return -1;
        if (r == 0) {
            usleep(1000);
            continue;
        }

        if (hdr.seq == want_seq) {
            if (hdr.len > payload_cap || (!out_payload && hdr.len)) return -1;
            if (out_hdr) *out_hdr = hdr;
            if (hdr.len) memcpy(out_payload, payload, hdr.len);
            return 1;
        }

        if (comp_pending_push(c, &hdr, payload) != 0) return -1;
    }
    return 0;
}

static inline void comp_conn_reset(comp_conn_t* c) {
    if (!c) return;
    memset(c, 0, sizeof(*c));
    c->connected = 0;
    c->fd_c2s_w = -1;
    c->fd_s2c_r = -1;
    c->input_ring_shm_fd = -1;
    c->input_ring_size_bytes = 0;
    c->input_ring_shm_name[0] = '\0';
    c->input_ring = 0;
    c->input_ring_enabled = 0;
    c->seq = 1;
    c->rx.r = 0;
    c->rx.w = 0;
    c->pending_r = 0;
    c->pending_w = 0;
}

static inline void comp_disconnect(comp_conn_t* c) {
    if (!c) return;
    c->connected = 0;
    comp_input_ring_close(c);
    if (c->fd_c2s_w >= 0) {
        close(c->fd_c2s_w);
        c->fd_c2s_w = -1;
    }
    if (c->fd_s2c_r >= 0) {
        close(c->fd_s2c_r);
        c->fd_s2c_r = -1;
    }
    c->rx.r = 0;
    c->rx.w = 0;
    c->pending_r = 0;
    c->pending_w = 0;
}

static inline int comp_connect(comp_conn_t* c, const char* endpoint_name) {
    if (!c) return -1;
    comp_conn_reset(c);

    int fds[2] = { -1, -1 };
    if (ipc_connect(endpoint_name, fds) != 0) {
        return -1;
    }

    c->fd_s2c_r = fds[0];
    c->fd_c2s_w = fds[1];
    c->connected = 1;
    c->seq = 1;
    return 0;
}

static inline int comp_wait_ack_or_error(comp_conn_t* c, uint32_t want_seq, uint16_t req_type, uint32_t surface_id, uint16_t* out_err_code, uint32_t max_iters);

static inline int comp_send_hello(comp_conn_t* c) {
    if (!c || !c->connected || c->fd_c2s_w < 0) return -1;

    comp_ipc_hello_t hello;
    hello.client_pid = (uint32_t)getpid();
    hello.reserved = 0;
    return comp_ipc_send(c->fd_c2s_w, (uint16_t)COMP_IPC_MSG_HELLO, c->seq++, &hello, (uint32_t)sizeof(hello));
}

static inline int comp_send_hello_sync(comp_conn_t* c, uint32_t max_iters, uint16_t* out_err_code) {
    if (!c || !c->connected || c->fd_c2s_w < 0) return -1;

    comp_ipc_hello_t hello;
    hello.client_pid = (uint32_t)getpid();
    hello.reserved = 0;

    uint32_t seq = c->seq++;
    if (comp_ipc_send(c->fd_c2s_w, (uint16_t)COMP_IPC_MSG_HELLO, seq, &hello, (uint32_t)sizeof(hello)) != 0) return -1;
    return comp_wait_ack_or_error(c, seq, (uint16_t)COMP_IPC_MSG_HELLO, 0u, out_err_code, max_iters);
}

static inline int comp_send_attach_shm_name(comp_conn_t* c,
                                           uint32_t surface_id,
                                           const char* shm_name,
                                           uint32_t size_bytes,
                                           uint32_t width,
                                           uint32_t height,
                                           uint32_t stride,
                                           uint32_t format) {
    if (!c || !c->connected || c->fd_c2s_w < 0) return -1;
    if (!shm_name) return -1;
    if (surface_id == 0) return -1;
    if (size_bytes == 0 || width == 0 || height == 0) return -1;
    if (stride == 0) stride = width;

    comp_ipc_attach_shm_name_t a;
    memset(&a, 0, sizeof(a));
    a.surface_id = surface_id;
    a.width = width;
    a.height = height;
    a.stride = stride;
    a.format = format;
    a.size_bytes = size_bytes;

    size_t n = strlen(shm_name);
    if (n == 0) return -1;
    if (n >= sizeof(a.shm_name)) return -1;
    memcpy(a.shm_name, shm_name, n + 1u);

    return comp_ipc_send(c->fd_c2s_w, (uint16_t)COMP_IPC_MSG_ATTACH_SHM_NAME, c->seq++, &a, (uint32_t)sizeof(a));
}

static inline int comp_send_commit(comp_conn_t* c, uint32_t surface_id, int32_t x, int32_t y, uint32_t flags) {
    if (!c || !c->connected || c->fd_c2s_w < 0) return -1;
    if (surface_id == 0) return -1;

    comp_ipc_commit_t cm;
    cm.surface_id = surface_id;
    cm.x = x;
    cm.y = y;
    cm.flags = flags;
    return comp_ipc_send(c->fd_c2s_w, (uint16_t)COMP_IPC_MSG_COMMIT, c->seq++, &cm, (uint32_t)sizeof(cm));
}

static inline int comp_send_destroy_surface(comp_conn_t* c, uint32_t surface_id, uint32_t flags) {
    if (!c || !c->connected || c->fd_c2s_w < 0) return -1;
    if (surface_id == 0) return -1;

    comp_ipc_destroy_surface_t d;
    d.surface_id = surface_id;
    d.flags = flags;
    return comp_ipc_send(c->fd_c2s_w, (uint16_t)COMP_IPC_MSG_DESTROY_SURFACE, c->seq++, &d, (uint32_t)sizeof(d));
}

static inline int comp_try_recv(comp_conn_t* c, comp_ipc_hdr_t* out_hdr, void* out_payload, uint32_t payload_cap) {
    if (!c || !c->connected || c->fd_s2c_r < 0) return -1;

    int pr = comp_pending_pop(c, out_hdr, out_payload, payload_cap);
    if (pr != 0) return pr;

    if (out_payload && payload_cap >= (uint32_t)sizeof(comp_ipc_input_t)) {
        comp_ipc_input_t ev;
        int rr = comp_input_ring_try_pop(c, &ev);
        if (rr == 1) {
            if (out_hdr) {
                out_hdr->magic = COMP_IPC_MAGIC;
                out_hdr->version = (uint16_t)COMP_IPC_VERSION;
                out_hdr->type = (uint16_t)COMP_IPC_MSG_INPUT;
                out_hdr->len = (uint32_t)sizeof(ev);
                out_hdr->seq = 0;
            }
            memcpy(out_payload, &ev, sizeof(ev));
            return 1;
        }
    }

    return comp_try_recv_raw(c, out_hdr, out_payload, payload_cap);
}

static inline int comp_wait_ack_or_error(comp_conn_t* c, uint32_t want_seq, uint16_t req_type, uint32_t surface_id, uint16_t* out_err_code, uint32_t max_iters) {
    if (out_err_code) *out_err_code = 0;

    uint8_t payload[COMP_IPC_MAX_PAYLOAD];
    comp_ipc_hdr_t hdr;
    for (;;) {
        int r = comp_wait_reply(c, want_seq, &hdr, payload, (uint32_t)sizeof(payload), max_iters);
        if (r < 0) return -1;
        if (r == 0) return -1;

        if (comp_is_ack_for(&hdr, payload, (uint32_t)req_type, surface_id)) {
            return 0;
        }

        uint16_t code = 0;
        if (comp_is_error_for(&hdr, payload, (uint32_t)req_type, surface_id, &code)) {
            if (out_err_code) *out_err_code = code;
            return -(int)code;
        }

        (void)comp_pending_push(c, &hdr, payload);
    }
}

static inline int comp_send_attach_shm_name_sync(comp_conn_t* c,
                                                 uint32_t surface_id,
                                                 const char* shm_name,
                                                 uint32_t size_bytes,
                                                 uint32_t width,
                                                 uint32_t height,
                                                 uint32_t stride,
                                                 uint32_t format,
                                                 uint32_t max_iters,
                                                 uint16_t* out_err_code) {
    if (!c || !c->connected || c->fd_c2s_w < 0) return -1;
    if (!shm_name) return -1;
    if (surface_id == 0) return -1;
    if (size_bytes == 0 || width == 0 || height == 0) return -1;
    if (stride == 0) stride = width;

    comp_ipc_attach_shm_name_t a;
    memset(&a, 0, sizeof(a));
    a.surface_id = surface_id;
    a.width = width;
    a.height = height;
    a.stride = stride;
    a.format = format;
    a.size_bytes = size_bytes;

    size_t n = strlen(shm_name);
    if (n == 0) return -1;
    if (n >= sizeof(a.shm_name)) return -1;
    memcpy(a.shm_name, shm_name, n + 1u);

    uint32_t seq = c->seq++;
    if (comp_ipc_send(c->fd_c2s_w, (uint16_t)COMP_IPC_MSG_ATTACH_SHM_NAME, seq, &a, (uint32_t)sizeof(a)) != 0) return -1;
    return comp_wait_ack_or_error(c, seq, (uint16_t)COMP_IPC_MSG_ATTACH_SHM_NAME, surface_id, out_err_code, max_iters);
}

static inline int comp_send_commit_sync(comp_conn_t* c, uint32_t surface_id, int32_t x, int32_t y, uint32_t flags, uint32_t max_iters, uint16_t* out_err_code) {
    if (!c || !c->connected || c->fd_c2s_w < 0) return -1;
    if (surface_id == 0) return -1;

    comp_ipc_commit_t cm;
    cm.surface_id = surface_id;
    cm.x = x;
    cm.y = y;
    cm.flags = flags | COMP_IPC_COMMIT_FLAG_ACK;

    uint32_t seq = c->seq++;
    if (comp_ipc_send(c->fd_c2s_w, (uint16_t)COMP_IPC_MSG_COMMIT, seq, &cm, (uint32_t)sizeof(cm)) != 0) return -1;
    return comp_wait_ack_or_error(c, seq, (uint16_t)COMP_IPC_MSG_COMMIT, surface_id, out_err_code, max_iters);
}

static inline int comp_send_destroy_surface_sync(comp_conn_t* c, uint32_t surface_id, uint32_t flags, uint32_t max_iters, uint16_t* out_err_code) {
    if (!c || !c->connected || c->fd_c2s_w < 0) return -1;
    if (surface_id == 0) return -1;

    comp_ipc_destroy_surface_t d;
    d.surface_id = surface_id;
    d.flags = flags;

    uint32_t seq = c->seq++;
    if (comp_ipc_send(c->fd_c2s_w, (uint16_t)COMP_IPC_MSG_DESTROY_SURFACE, seq, &d, (uint32_t)sizeof(d)) != 0) return -1;
    return comp_wait_ack_or_error(c, seq, (uint16_t)COMP_IPC_MSG_DESTROY_SURFACE, surface_id, out_err_code, max_iters);
}

static inline int comp_wm_connect(comp_conn_t* c) {
    return comp_connect(c, "compositor_wm");
}

static inline int comp_wm_send_cmd(comp_conn_t* c, uint32_t kind, uint32_t client_id, uint32_t surface_id, int32_t x, int32_t y, uint32_t flags) {
    if (!c || !c->connected || c->fd_c2s_w < 0) return -1;
    comp_ipc_wm_cmd_t cmd;
    cmd.kind = kind;
    cmd.client_id = client_id;
    cmd.surface_id = surface_id;
    cmd.x = x;
    cmd.y = y;
    cmd.flags = flags;
    return comp_ipc_send(c->fd_c2s_w, (uint16_t)COMP_IPC_MSG_WM_CMD, c->seq++, &cmd, (uint32_t)sizeof(cmd));
}

static inline int comp_wm_focus(comp_conn_t* c, uint32_t client_id, uint32_t surface_id) {
    return comp_wm_send_cmd(c, COMP_WM_CMD_FOCUS, client_id, surface_id, 0, 0, 0);
}

static inline int comp_wm_raise(comp_conn_t* c, uint32_t client_id, uint32_t surface_id) {
    return comp_wm_send_cmd(c, COMP_WM_CMD_RAISE, client_id, surface_id, 0, 0, 0);
}

static inline int comp_wm_move(comp_conn_t* c, uint32_t client_id, uint32_t surface_id, int32_t x, int32_t y) {
    return comp_wm_send_cmd(c, COMP_WM_CMD_MOVE, client_id, surface_id, x, y, 0);
}

static inline int comp_wm_resize(comp_conn_t* c, uint32_t client_id, uint32_t surface_id, int32_t w, int32_t h) {
    return comp_wm_send_cmd(c, COMP_WM_CMD_RESIZE, client_id, surface_id, w, h, 0);
}

static inline int comp_wm_preview_rect(comp_conn_t* c, uint32_t client_id, uint32_t surface_id, int32_t w, int32_t h) {
    return comp_wm_send_cmd(c, COMP_WM_CMD_PREVIEW_RECT, client_id, surface_id, w, h, 0);
}

static inline int comp_wm_preview_clear(comp_conn_t* c, uint32_t client_id, uint32_t surface_id) {
    return comp_wm_send_cmd(c, COMP_WM_CMD_PREVIEW_CLEAR, client_id, surface_id, 0, 0, 0);
}

static inline int comp_wm_close(comp_conn_t* c, uint32_t client_id, uint32_t surface_id) {
    return comp_wm_send_cmd(c, COMP_WM_CMD_CLOSE, client_id, surface_id, 0, 0, 0);
}

static inline int comp_wm_exit(comp_conn_t* c) {
    return comp_wm_send_cmd(c, COMP_WM_CMD_EXIT, COMP_WM_CLIENT_NONE, 0, 0, 0, 0);
}

static inline int comp_wm_pointer_grab(comp_conn_t* c, uint32_t client_id, uint32_t surface_id, int enable) {
    return comp_wm_send_cmd(c, COMP_WM_CMD_POINTER_GRAB, client_id, surface_id, 0, 0, enable ? 1u : 0u);
}

static inline int comp_wm_keyboard_grab(comp_conn_t* c, int enable) {
    return comp_wm_send_cmd(c, COMP_WM_CMD_KEYBOARD_GRAB, COMP_WM_CLIENT_NONE, 0, 0, 0, enable ? 1u : 0u);
}

static inline int comp_wm_is_event(const comp_ipc_hdr_t* hdr) {
    return hdr && hdr->type == (uint16_t)COMP_IPC_MSG_WM_EVENT && hdr->len == (uint32_t)sizeof(comp_ipc_wm_event_t);
}

#endif
