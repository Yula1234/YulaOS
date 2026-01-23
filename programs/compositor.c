// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#include <yula.h>
#include <font.h>
#include <comp_ipc.h>

static volatile int g_should_exit;
static volatile int g_fb_released;

static volatile int g_dbg_curr_pid = -1;
static volatile int g_dbg_last_rx_pid = -1;
static volatile uint16_t g_dbg_last_rx_type = 0;
static volatile uint32_t g_dbg_last_rx_seq = 0;
static volatile uint32_t g_dbg_last_rx_surface_id = 0;

static volatile int g_dbg_last_err_pid = -1;
static volatile uint16_t g_dbg_last_err_req_type = 0;
static volatile uint16_t g_dbg_last_err_code = 0;
static volatile uint32_t g_dbg_last_err_surface_id = 0;
static volatile uint32_t g_dbg_last_err_detail = 0;

static volatile int g_dbg_bar_rx_pid = -1;
static volatile uint16_t g_dbg_bar_rx_type = 0;
static volatile uint32_t g_dbg_bar_rx_seq = 0;

static volatile int g_dbg_bar_err_pid = -1;
static volatile uint16_t g_dbg_bar_err_req_type = 0;
static volatile uint16_t g_dbg_bar_err_code = 0;
static volatile uint32_t g_dbg_bar_err_seq = 0;
static volatile uint32_t g_dbg_bar_err_detail = 0;

static uint32_t g_commit_gen = 1;

static void on_signal(int sig) {
    (void)sig;
    if (!g_fb_released) {
        fb_release();
        g_fb_released = 1;
    }
    g_should_exit = 1;

    sigreturn();
    for (;;) {
    }
}

static inline void dbg_write(const char* s) {
    if (!s) return;
    write(1, s, (uint32_t)strlen(s));
}

static int pipe_try_write_frame(int fd, const void* buf, uint32_t size, int essential) {
    if (fd < 0 || !buf || size == 0) return -1;

    const uint8_t* p = (const uint8_t*)buf;
    uint32_t off = 0;
    int tries = 0;

    const int max_tries_initial = essential ? 256 : 1;
    const int max_tries_partial = 4096;

    while (off < size) {
        int wn = pipe_try_write(fd, p + off, size - off);
        if (wn < 0) return -1;
        if (wn == 0) {
            if (off == 0 && !essential) return 0;

            const int max_tries = (off == 0) ? max_tries_initial : max_tries_partial;
            tries++;
            if (tries >= max_tries) {
                return (off == 0) ? 0 : -1;
            }
            usleep(1000);
            continue;
        }

        off += (uint32_t)wn;
        tries = 0;
    }

    return 1;
}

static int comp_send_reply(int fd, uint16_t type, uint32_t seq, const void* payload, uint32_t payload_len) {
    if (fd < 0) return -1;
    return comp_ipc_send(fd, type, seq, payload, payload_len);
}

static void comp_send_ack(int fd, uint32_t seq, uint16_t req_type, uint32_t surface_id, uint32_t flags) {
    comp_ipc_ack_t a;
    a.req_type = req_type;
    a.reserved = 0;
    a.surface_id = surface_id;
    a.flags = flags;
    (void)comp_send_reply(fd, (uint16_t)COMP_IPC_MSG_ACK, seq, &a, (uint32_t)sizeof(a));
}

static void comp_send_error(int fd, uint32_t seq, uint16_t req_type, uint16_t code, uint32_t surface_id, uint32_t detail) {
    comp_ipc_error_t e;
    e.req_type = req_type;
    e.code = code;
    e.surface_id = surface_id;
    e.detail = detail;
    g_dbg_last_err_pid = g_dbg_curr_pid;
    g_dbg_last_err_req_type = req_type;
    g_dbg_last_err_code = code;
    g_dbg_last_err_surface_id = surface_id;
    g_dbg_last_err_detail = detail;
    if (surface_id == 0x80000001u) {
        g_dbg_bar_err_pid = g_dbg_curr_pid;
        g_dbg_bar_err_req_type = req_type;
        g_dbg_bar_err_code = code;
        g_dbg_bar_err_seq = seq;
        g_dbg_bar_err_detail = detail;
    }
    (void)comp_send_reply(fd, (uint16_t)COMP_IPC_MSG_ERROR, seq, &e, (uint32_t)sizeof(e));
}

static inline void put_pixel(uint32_t* fb, int stride, int w, int h, int x, int y, uint32_t color) {
    if ((unsigned)x >= (unsigned)w || (unsigned)y >= (unsigned)h) return;
    fb[(size_t)y * (size_t)stride + (size_t)x] = color;
}

static void comp_draw_char(uint32_t* fb, int stride, int w, int h, int x, int y, char c, uint32_t color) {
    if ((unsigned char)c > 127) c = '?';
    const uint8_t* glyph = font8x8_basic[(int)(unsigned char)c];

    for (int row = 0; row < 8; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            if ((bits >> (7 - col)) & 1u) {
                put_pixel(fb, stride, w, h, x + col, y + row, color);
            }
        }
    }
}

static void comp_draw_string(uint32_t* fb, int stride, int w, int h, int x, int y, const char* s, uint32_t color) {
    if (!s) return;
    while (*s) {
        comp_draw_char(fb, stride, w, h, x, y, *s, color);
        x += 8;
        s++;
    }
}

static void fill_rect(uint32_t* fb, int stride, int w, int h, int x, int y, int rw, int rh, uint32_t color) {
    if (rw <= 0 || rh <= 0) return;

    int x0 = x;
    int y0 = y;
    int x1 = x + rw;
    int y1 = y + rh;

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > w) x1 = w;
    if (y1 > h) y1 = h;

    if (x0 >= x1 || y0 >= y1) return;

    for (int yy = y0; yy < y1; yy++) {
        uint32_t* row = fb + (size_t)yy * (size_t)stride;
        for (int xx = x0; xx < x1; xx++) {
            row[xx] = color;
        }
    }
}

static void draw_cursor(uint32_t* fb, int stride, int w, int h, int x, int y) {
    const uint32_t c1 = 0xFFFFFF;
    const uint32_t c2 = 0x000000;

    for (int i = -7; i <= 7; i++) {
        put_pixel(fb, stride, w, h, x + i, y, c1);
        put_pixel(fb, stride, w, h, x, y + i, c1);
        put_pixel(fb, stride, w, h, x + i, y + 1, c2);
        put_pixel(fb, stride, w, h, x + 1, y + i, c2);
    }

    fill_rect(fb, stride, w, h, x - 1, y - 1, 3, 3, 0xFF0000);
}

static void draw_frame_rect(uint32_t* fb, int stride, int w, int h, int x, int y, int rw, int rh, int t, uint32_t color) {
    if (!fb) return;
    if (rw <= 0 || rh <= 0) return;
    if (t <= 0) return;
    if (rw <= t * 2 || rh <= t * 2) return;

    fill_rect(fb, stride, w, h, x, y, rw, t, color);
    fill_rect(fb, stride, w, h, x, y + rh - t, rw, t, color);
    fill_rect(fb, stride, w, h, x, y, t, rh, color);
    fill_rect(fb, stride, w, h, x + rw - t, y, t, rh, color);
}

typedef struct {
    uint8_t buf[4096];
    uint32_t r;
    uint32_t w;
} ipc_rx_ring_t;

static inline uint32_t ipc_rx_count(const ipc_rx_ring_t* q) {
    return q->w - q->r;
}

static inline void ipc_rx_reset(ipc_rx_ring_t* q) {
    q->r = 0;
    q->w = 0;
}

static void ipc_rx_push(ipc_rx_ring_t* q, const uint8_t* src, uint32_t n) {
    if (!q || !src || n == 0) return;

    const uint32_t cap = (uint32_t)sizeof(q->buf);
    uint32_t count = ipc_rx_count(q);

    if (n > cap) {
        src += (n - cap);
        n = cap;
        q->r = 0;
        q->w = 0;
        count = 0;
    }

    if (count + n > cap) {
        uint32_t drop = (count + n) - cap;
        q->r += drop;
    }

    uint32_t mask = cap - 1u;
    uint32_t wi = q->w & mask;
    uint32_t first = cap - wi;
    if (first > n) first = n;
    memcpy(&q->buf[wi], src, first);
    if (n > first) {
        memcpy(&q->buf[0], src + first, n - first);
    }
    q->w += n;
}

static void ipc_rx_peek(const ipc_rx_ring_t* q, uint32_t off, void* dst, uint32_t n) {
    uint8_t* out = (uint8_t*)dst;
    const uint32_t cap = (uint32_t)sizeof(q->buf);
    uint32_t mask = cap - 1u;
    uint32_t ri = (q->r + off) & mask;
    uint32_t first = cap - ri;
    if (first > n) first = n;
    memcpy(out, &q->buf[ri], first);
    if (n > first) {
        memcpy(out + first, &q->buf[0], n - first);
    }
}

static inline void ipc_rx_drop(ipc_rx_ring_t* q, uint32_t n) {
    uint32_t count = ipc_rx_count(q);
    if (n > count) n = count;
    q->r += n;
}

static void blit_surface(uint32_t* dst, int dst_stride, int dst_w, int dst_h, int dx, int dy,
                         const uint32_t* src, int src_stride, int src_w, int src_h) {
    if (!dst || !src) return;
    if (dst_w <= 0 || dst_h <= 0 || src_w <= 0 || src_h <= 0) return;

    if (dx >= dst_w || dy >= dst_h) return;
    if (dx < 0) return;
    if (dy < 0) return;

    int copy_w = src_w;
    int copy_h = src_h;
    if (dx + copy_w > dst_w) copy_w = dst_w - dx;
    if (dy + copy_h > dst_h) copy_h = dst_h - dy;
    if (copy_w <= 0 || copy_h <= 0) return;

    for (int y = 0; y < copy_h; y++) {
        uint32_t* drow = dst + (size_t)(dy + y) * (size_t)dst_stride + (size_t)dx;
        const uint32_t* srow = src + (size_t)y * (size_t)src_stride;
        memcpy(drow, srow, (size_t)copy_w * 4u);
    }
}

#define COMP_MAX_SURFACES 8
#define COMP_MAX_CLIENTS  8

typedef struct {
    int shm_fd;
    uint32_t* pixels;
    uint32_t size_bytes;
    int w;
    int h;
    int stride;
} comp_buffer_t;

typedef struct {
    int in_use;
    uint32_t id;
    int attached;
    int committed;
    uint32_t commit_gen;

    uint32_t z;

    int x;
    int y;

    uint32_t* pixels;
    int w;
    int h;
    int stride;

    int owns_buffer;
    int shm_fd;
    uint32_t size_bytes;
    char shm_name[32];
} comp_surface_t;

typedef struct {
    int connected;
    int pid;
    int fd_c2s;
    int fd_s2c;
    ipc_rx_ring_t rx;

    uint32_t focus_surface_id;
    uint32_t pointer_grab_surface_id;
    int pointer_grab_active;
    uint32_t prev_buttons;

    uint32_t last_mx;
    uint32_t last_my;
    uint32_t last_mb;
    uint32_t last_input_surface_id;
    uint32_t seq_out;

    uint32_t z_counter;

    comp_surface_t surfaces[COMP_MAX_SURFACES];
} comp_client_t;

typedef struct {
    int focus_client;
    uint32_t focus_surface_id;

    int grab_active;
    int grab_client;
    uint32_t grab_surface_id;

    int wm_pointer_grab_active;
    int wm_pointer_grab_client;
    uint32_t wm_pointer_grab_surface_id;

    uint32_t prev_buttons;

    uint32_t wm_last_mx;
    uint32_t wm_last_my;
    uint32_t wm_last_mb;
    int wm_last_client;
    uint32_t wm_last_surface_id;

    uint32_t last_mx;
    uint32_t last_my;
    uint32_t last_mb;
    int last_client;
    uint32_t last_surface_id;
} comp_input_state_t;

typedef struct {
    int active;
    uint32_t client_id;
    uint32_t surface_id;
    int32_t w;
    int32_t h;
} comp_preview_t;

typedef struct {
    int connected;
    int fd_c2s;
    int fd_s2c;
    ipc_rx_ring_t rx;
    uint32_t seq_out;
} wm_conn_t;

static comp_surface_t* comp_client_surface_get(comp_client_t* c, uint32_t id, int create);

static void wm_disconnect(wm_conn_t* w) {
    if (!w) return;
    w->connected = 0;
    if (w->fd_c2s >= 0) {
        close(w->fd_c2s);
        w->fd_c2s = -1;
    }
    if (w->fd_s2c >= 0) {
        close(w->fd_s2c);
        w->fd_s2c = -1;
    }
    ipc_rx_reset(&w->rx);
    w->seq_out = 1;
}

static void wm_init(wm_conn_t* w, int fd_c2s, int fd_s2c) {
    if (!w) return;
    memset(w, 0, sizeof(*w));
    w->connected = 1;
    w->fd_c2s = fd_c2s;
    w->fd_s2c = fd_s2c;
    ipc_rx_reset(&w->rx);
    w->seq_out = 1;
}

static int wm_send_event(wm_conn_t* w, const comp_ipc_wm_event_t* ev, int essential) {
    if (!w || !w->connected || w->fd_s2c < 0 || !ev) return -1;

    comp_ipc_hdr_t hdr;
    hdr.magic = COMP_IPC_MAGIC;
    hdr.version = (uint16_t)COMP_IPC_VERSION;
    hdr.type = (uint16_t)COMP_IPC_MSG_WM_EVENT;
    hdr.len = (uint32_t)sizeof(*ev);
    hdr.seq = w->seq_out++;

    uint8_t frame[sizeof(hdr) + sizeof(*ev)];
    memcpy(frame, &hdr, sizeof(hdr));
    memcpy(frame + sizeof(hdr), ev, sizeof(*ev));

    int wr = pipe_try_write_frame(w->fd_s2c, frame, (uint32_t)sizeof(frame), essential);
    if (wr < 0) return -1;
    if (essential && wr == 0) return -1;
    return 0;
}

static void wm_replay_state(wm_conn_t* wm, const comp_client_t* clients, int nclients) {
    if (!wm || !wm->connected) return;
    if (!clients || nclients <= 0) return;

    for (int ci = 0; ci < nclients; ci++) {
        const comp_client_t* c = &clients[ci];
        if (!c->connected) continue;
        for (int si = 0; si < COMP_MAX_SURFACES; si++) {
            const comp_surface_t* s = &c->surfaces[si];
            if (!s->in_use || !s->attached || !s->committed) continue;

            comp_ipc_wm_event_t ev;
            memset(&ev, 0, sizeof(ev));
            ev.kind = COMP_WM_EVENT_MAP;
            ev.client_id = (uint32_t)ci;
            ev.surface_id = s->id;
            ev.sx = (int32_t)s->x;
            ev.sy = (int32_t)s->y;
            ev.sw = (uint32_t)s->w;
            ev.sh = (uint32_t)s->h;
            ev.flags = COMP_WM_EVENT_FLAG_REPLAY;
            if (wm_send_event(wm, &ev, 1) < 0) {
                wm_disconnect(wm);
                return;
            }
        }
    }
}

static void wm_pump(wm_conn_t* w, comp_client_t* clients, int nclients, comp_input_state_t* input, uint32_t* z_counter, comp_preview_t* preview, int* preview_dirty) {
    if (!w || !w->connected || w->fd_c2s < 0) return;
    if (!clients || !input || !z_counter) return;

    int saw_eof = 0;

    for (;;) {
        uint8_t tmp[128];
        int rn = pipe_try_read(w->fd_c2s, tmp, (uint32_t)sizeof(tmp));
        if (rn < 0) {
            saw_eof = 1;
            break;
        }
        if (rn == 0) break;
        ipc_rx_push(&w->rx, tmp, (uint32_t)rn);
    }

    for (;;) {
        uint32_t avail = ipc_rx_count(&w->rx);
        if (avail < 4) break;

        uint32_t magic;
        ipc_rx_peek(&w->rx, 0, &magic, 4);
        if (magic != COMP_IPC_MAGIC) {
            ipc_rx_drop(&w->rx, 1);
            continue;
        }

        if (avail < (uint32_t)sizeof(comp_ipc_hdr_t)) break;

        comp_ipc_hdr_t hdr;
        ipc_rx_peek(&w->rx, 0, &hdr, (uint32_t)sizeof(hdr));

        if (hdr.version != COMP_IPC_VERSION || hdr.len > COMP_IPC_MAX_PAYLOAD) {
            ipc_rx_drop(&w->rx, 1);
            continue;
        }

        uint32_t frame_len = (uint32_t)sizeof(hdr) + hdr.len;
        if (avail < frame_len) break;

        ipc_rx_drop(&w->rx, (uint32_t)sizeof(hdr));
        uint8_t payload[COMP_IPC_MAX_PAYLOAD];
        if (hdr.len) {
            ipc_rx_peek(&w->rx, 0, payload, hdr.len);
            ipc_rx_drop(&w->rx, hdr.len);
        }

        if (hdr.type == COMP_IPC_MSG_WM_CMD && hdr.len == (uint32_t)sizeof(comp_ipc_wm_cmd_t)) {
            comp_ipc_wm_cmd_t cmd;
            memcpy(&cmd, payload, sizeof(cmd));

            if (cmd.kind == COMP_WM_CMD_POINTER_GRAB) {
                if (cmd.flags & 1u) {
                    if (cmd.client_id >= (uint32_t)nclients || cmd.surface_id == 0) continue;
                    comp_client_t* c = &clients[(int)cmd.client_id];
                    if (!c->connected) continue;
                    comp_surface_t* s = comp_client_surface_get(c, cmd.surface_id, 0);
                    if (!s || !s->attached || !s->committed) continue;
                    input->wm_pointer_grab_active = 1;
                    input->wm_pointer_grab_client = (int)cmd.client_id;
                    input->wm_pointer_grab_surface_id = cmd.surface_id;
                } else {
                    input->wm_pointer_grab_active = 0;
                    input->wm_pointer_grab_client = -1;
                    input->wm_pointer_grab_surface_id = 0;
                }
                continue;
            }

            if (cmd.client_id >= (uint32_t)nclients || cmd.surface_id == 0) continue;

            comp_client_t* c = &clients[(int)cmd.client_id];
            if (!c->connected) continue;

            comp_surface_t* s = comp_client_surface_get(c, cmd.surface_id, 0);
            if (!s || !s->attached || !s->committed) continue;

            if (cmd.kind == COMP_WM_CMD_FOCUS) {
                input->focus_client = (int)cmd.client_id;
                input->focus_surface_id = cmd.surface_id;
            } else if (cmd.kind == COMP_WM_CMD_RAISE) {
                s->z = ++(*z_counter);
            } else if (cmd.kind == COMP_WM_CMD_MOVE) {
                s->x = (int)cmd.x;
                s->y = (int)cmd.y;
            } else if (cmd.kind == COMP_WM_CMD_RESIZE) {
                if (cmd.x <= 0 || cmd.y <= 0) continue;
                if (c->fd_s2c < 0) continue;

                comp_ipc_input_t in;
                in.surface_id = cmd.surface_id;
                in.kind = COMP_IPC_INPUT_RESIZE;
                in.x = cmd.x;
                in.y = cmd.y;
                in.buttons = 0;
                in.keycode = 0;
                in.key_state = 0;

                comp_ipc_hdr_t oh;
                oh.magic = COMP_IPC_MAGIC;
                oh.version = (uint16_t)COMP_IPC_VERSION;
                oh.type = (uint16_t)COMP_IPC_MSG_INPUT;
                oh.len = (uint32_t)sizeof(in);
                oh.seq = c->seq_out++;

                uint8_t frame[sizeof(oh) + sizeof(in)];
                memcpy(frame, &oh, sizeof(oh));
                memcpy(frame + sizeof(oh), &in, sizeof(in));
                (void)pipe_try_write_frame(c->fd_s2c, frame, (uint32_t)sizeof(frame), 1);
            } else if (cmd.kind == COMP_WM_CMD_PREVIEW_RECT) {
                if (!preview) continue;
                if (cmd.x <= 0 || cmd.y <= 0) continue;

                const int32_t nw = cmd.x;
                const int32_t nh = cmd.y;

                if (!preview->active || preview->client_id != cmd.client_id || preview->surface_id != cmd.surface_id || preview->w != nw || preview->h != nh) {
                    preview->active = 1;
                    preview->client_id = cmd.client_id;
                    preview->surface_id = cmd.surface_id;
                    preview->w = nw;
                    preview->h = nh;
                    if (preview_dirty) *preview_dirty = 1;
                }
            } else if (cmd.kind == COMP_WM_CMD_PREVIEW_CLEAR) {
                if (!preview) continue;
                if (preview->active && preview->client_id == cmd.client_id && preview->surface_id == cmd.surface_id) {
                    preview->active = 0;
                    if (preview_dirty) *preview_dirty = 1;
                }
            } else if (cmd.kind == COMP_WM_CMD_CLOSE) {
                int pid = c->pid;
                if (pid > 0) {
                    if (input->focus_client == (int)cmd.client_id) {
                        input->focus_client = -1;
                        input->focus_surface_id = 0;
                    }
                    (void)syscall(9, pid, 0, 0);
                }
            }
        }
    }

    if (saw_eof) {
        input->wm_pointer_grab_active = 0;
        input->wm_pointer_grab_client = -1;
        input->wm_pointer_grab_surface_id = 0;
        wm_disconnect(w);
    }
}

static void comp_buffer_destroy(comp_buffer_t* b) {
    if (!b) return;
    if (b->pixels) {
        munmap((void*)b->pixels, b->size_bytes);
        b->pixels = 0;
    }
    if (b->shm_fd >= 0) {
        close(b->shm_fd);
        b->shm_fd = -1;
    }
    b->size_bytes = 0;
    b->w = 0;
    b->h = 0;
    b->stride = 0;
}

static void comp_client_disconnect(comp_client_t* c) {
    if (!c) return;
    c->connected = 0;
    if (c->fd_c2s >= 0) {
        close(c->fd_c2s);
        c->fd_c2s = -1;
    }
    if (c->fd_s2c >= 0) {
        close(c->fd_s2c);
        c->fd_s2c = -1;
    }
    ipc_rx_reset(&c->rx);
    c->focus_surface_id = 0;
    c->pointer_grab_surface_id = 0;
    c->pointer_grab_active = 0;
    c->prev_buttons = 0;
    c->last_mx = 0xFFFFFFFFu;
    c->last_my = 0xFFFFFFFFu;
    c->last_mb = 0xFFFFFFFFu;
    c->last_input_surface_id = 0xFFFFFFFFu;
    c->seq_out = 1;
    c->z_counter = 1;
    for (int i = 0; i < COMP_MAX_SURFACES; i++) {
        comp_surface_t* s = &c->surfaces[i];
        if (s->owns_buffer) {
            if (s->pixels && s->size_bytes) {
                munmap((void*)s->pixels, s->size_bytes);
            }
            if (s->shm_fd >= 0) {
                close(s->shm_fd);
            }
        }
        memset(s, 0, sizeof(*s));
        s->shm_fd = -1;
    }
}

static comp_surface_t* comp_client_surface_get(comp_client_t* c, uint32_t id, int create) {
    if (!c || id == 0) return 0;

    for (int i = 0; i < COMP_MAX_SURFACES; i++) {
        if (c->surfaces[i].in_use && c->surfaces[i].id == id) {
            return &c->surfaces[i];
        }
    }

    if (!create) return 0;

    for (int i = 0; i < COMP_MAX_SURFACES; i++) {
        if (!c->surfaces[i].in_use) {
            comp_surface_t* s = &c->surfaces[i];
            memset(s, 0, sizeof(*s));
            s->in_use = 1;
            s->id = id;
            s->shm_fd = -1;
            return s;
        }
    }
    return 0;
}

static void comp_client_init(comp_client_t* c, int pid, int fd_c2s, int fd_s2c) {
    if (!c) return;
    memset(c, 0, sizeof(*c));
    c->connected = 1;
    c->pid = pid;
    c->fd_c2s = fd_c2s;
    c->fd_s2c = fd_s2c;
    ipc_rx_reset(&c->rx);
    c->focus_surface_id = 0;
    c->pointer_grab_surface_id = 0;
    c->pointer_grab_active = 0;
    c->prev_buttons = 0;
    c->last_mx = 0xFFFFFFFFu;
    c->last_my = 0xFFFFFFFFu;
    c->last_mb = 0xFFFFFFFFu;
    c->last_input_surface_id = 0xFFFFFFFFu;
    c->seq_out = 1;
    c->z_counter = 1;
}

static int comp_surface_can_receive(const comp_surface_t* s) {
    if (!s || !s->in_use || !s->attached || !s->committed) return 0;
    if (!s->pixels || s->w <= 0 || s->h <= 0 || s->stride <= 0) return 0;
    return 1;
}

static int comp_surface_contains_point(const comp_surface_t* s, int x, int y) {
    if (!comp_surface_can_receive(s)) return 0;
    if (x < s->x || y < s->y) return 0;
    if (x >= (s->x + s->w) || y >= (s->y + s->h)) return 0;
    return 1;
}

static comp_surface_t* comp_client_surface_find(const comp_client_t* c, uint32_t id) {
    if (!c || id == 0) return 0;
    for (int i = 0; i < COMP_MAX_SURFACES; i++) {
        const comp_surface_t* s = &c->surfaces[i];
        if (s->in_use && s->id == id) return (comp_surface_t*)s;
    }
    return 0;
}

static int comp_client_surface_id_valid(const comp_client_t* c, uint32_t id) {
    comp_surface_t* s = comp_client_surface_find(c, id);
    return s ? comp_surface_can_receive(s) : 0;
}

static int comp_pick_surface_at(comp_client_t* clients, int nclients, int x, int y, int* out_client, uint32_t* out_sid, comp_surface_t** out_s) {
    if (out_client) *out_client = -1;
    if (out_sid) *out_sid = 0;
    if (out_s) *out_s = 0;

    uint32_t best_z = 0;
    int best_ci = -1;
    uint32_t best_sid = 0;
    comp_surface_t* best_s = 0;

    for (int ci = 0; ci < nclients; ci++) {
        comp_client_t* c = &clients[ci];
        if (!c->connected) continue;
        for (int i = 0; i < COMP_MAX_SURFACES; i++) {
            comp_surface_t* s = &c->surfaces[i];
            if (!comp_surface_contains_point(s, x, y)) continue;
            if (!best_s || s->z >= best_z) {
                best_z = s->z;
                best_ci = ci;
                best_sid = s->id;
                best_s = s;
            }
        }
    }

    if (!best_s) return 0;
    if (out_client) *out_client = best_ci;
    if (out_sid) *out_sid = best_sid;
    if (out_s) *out_s = best_s;
    return 1;
}

static void comp_input_state_init(comp_input_state_t* st) {
    if (!st) return;
    memset(st, 0, sizeof(*st));
    st->focus_client = -1;
    st->grab_client = -1;
    st->grab_active = 0;
    st->wm_pointer_grab_active = 0;
    st->wm_pointer_grab_client = -1;
    st->wm_pointer_grab_surface_id = 0;
    st->last_client = -1;
    st->wm_last_client = -1;
    st->last_mx = 0xFFFFFFFFu;
    st->last_my = 0xFFFFFFFFu;
    st->last_mb = 0xFFFFFFFFu;
    st->last_surface_id = 0u;
    st->wm_last_mx = 0xFFFFFFFFu;
    st->wm_last_my = 0xFFFFFFFFu;
    st->wm_last_mb = 0xFFFFFFFFu;
    st->wm_last_surface_id = 0u;
}

static void comp_send_wm_pointer(wm_conn_t* wm, comp_client_t* clients, int nclients, comp_input_state_t* st, const mouse_state_t* ms) {
    if (!wm || !wm->connected) return;
    if (!clients || nclients <= 0) return;
    if (!st || !ms) return;

    uint32_t mx = (uint32_t)ms->x;
    uint32_t my = (uint32_t)ms->y;
    uint32_t mb = (uint32_t)ms->buttons;

    int ci = -1;
    uint32_t sid = 0;
    comp_surface_t* s = 0;

    if (st->wm_pointer_grab_active) {
        if (st->wm_pointer_grab_client >= 0 && st->wm_pointer_grab_client < nclients &&
            clients[st->wm_pointer_grab_client].connected &&
            comp_client_surface_id_valid(&clients[st->wm_pointer_grab_client], st->wm_pointer_grab_surface_id)) {
            ci = st->wm_pointer_grab_client;
            sid = st->wm_pointer_grab_surface_id;
            s = comp_client_surface_find(&clients[ci], sid);
        } else {
            st->wm_pointer_grab_active = 0;
            st->wm_pointer_grab_client = -1;
            st->wm_pointer_grab_surface_id = 0;
        }
    }

    if (ci < 0 || sid == 0) {
        if (st->grab_active && st->grab_client >= 0 && st->grab_client < nclients &&
            clients[st->grab_client].connected &&
            comp_client_surface_id_valid(&clients[st->grab_client], st->grab_surface_id)) {
            ci = st->grab_client;
            sid = st->grab_surface_id;
            s = comp_client_surface_find(&clients[ci], sid);
        } else {
            (void)comp_pick_surface_at(clients, nclients, ms->x, ms->y, &ci, &sid, &s);
        }
    }

    if (mx == st->wm_last_mx && my == st->wm_last_my && mb == st->wm_last_mb && ci == st->wm_last_client && sid == st->wm_last_surface_id) {
        return;
    }

    st->wm_last_mx = mx;
    st->wm_last_my = my;
    st->wm_last_mb = mb;
    st->wm_last_client = ci;
    st->wm_last_surface_id = sid;

    comp_ipc_wm_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind = COMP_WM_EVENT_POINTER;
    ev.px = (int32_t)ms->x;
    ev.py = (int32_t)ms->y;
    ev.buttons = (uint32_t)ms->buttons;

    if (ci < 0 || sid == 0) {
        ev.client_id = COMP_WM_CLIENT_NONE;
        ev.surface_id = 0;
        ev.flags = COMP_WM_EVENT_FLAG_BACKGROUND;
    } else {
        ev.client_id = (uint32_t)ci;
        ev.surface_id = sid;
        if (s && s->attached && s->committed) {
            ev.sx = (int32_t)s->x;
            ev.sy = (int32_t)s->y;
            ev.sw = (uint32_t)s->w;
            ev.sh = (uint32_t)s->h;
        }
        ev.flags = 0;
    }

    if (wm_send_event(wm, &ev, 0) < 0) {
        wm_disconnect(wm);
        st->wm_pointer_grab_active = 0;
        st->wm_pointer_grab_client = -1;
        st->wm_pointer_grab_surface_id = 0;
    }
}

static void comp_update_focus(comp_client_t* clients, int nclients, comp_input_state_t* st, const mouse_state_t* ms, uint32_t* z_counter, wm_conn_t* wm) {
    if (!st || !ms || !z_counter) return;

    const uint32_t btn = (uint32_t)ms->buttons;
    const uint32_t left_mask = 1u;
    const int pressed = (btn & left_mask) && ((st->prev_buttons & left_mask) == 0);

    if (st->grab_active) {
        if (st->grab_client < 0 || st->grab_client >= nclients) {
            st->grab_active = 0;
            st->grab_client = -1;
            st->grab_surface_id = 0;
        } else if (!clients[st->grab_client].connected || !comp_client_surface_id_valid(&clients[st->grab_client], st->grab_surface_id)) {
            st->grab_active = 0;
            st->grab_client = -1;
            st->grab_surface_id = 0;
        }
    }

    if (st->focus_client >= 0 && st->focus_client < nclients) {
        if (!clients[st->focus_client].connected || !comp_client_surface_id_valid(&clients[st->focus_client], st->focus_surface_id)) {
            st->focus_client = -1;
            st->focus_surface_id = 0;
        }
    } else {
        st->focus_client = -1;
        st->focus_surface_id = 0;
    }

    if (pressed) {
        int ci = -1;
        uint32_t sid = 0;
        comp_surface_t* s = 0;
        if (comp_pick_surface_at(clients, nclients, ms->x, ms->y, &ci, &sid, &s)) {
            st->grab_active = 1;
            st->grab_client = ci;
            st->grab_surface_id = sid;

            if (wm && wm->connected) {
                comp_ipc_wm_event_t ev;
                memset(&ev, 0, sizeof(ev));
                ev.kind = COMP_WM_EVENT_CLICK;
                ev.client_id = (uint32_t)ci;
                ev.surface_id = sid;
                ev.sx = s ? (int32_t)s->x : 0;
                ev.sy = s ? (int32_t)s->y : 0;
                ev.sw = s ? (uint32_t)s->w : 0u;
                ev.sh = s ? (uint32_t)s->h : 0u;
                ev.px = (int32_t)ms->x;
                ev.py = (int32_t)ms->y;
                ev.buttons = (uint32_t)ms->buttons;
                ev.flags = 0;
                if (wm_send_event(wm, &ev, 1) < 0) {
                    wm_disconnect(wm);
                }
            } else {
                st->focus_client = ci;
                st->focus_surface_id = sid;
                if (s) {
                    s->z = ++(*z_counter);
                }
            }
        } else {
            st->grab_active = 0;
            st->grab_client = -1;
            st->grab_surface_id = 0;
            if (wm && wm->connected) {
                comp_ipc_wm_event_t ev;
                memset(&ev, 0, sizeof(ev));
                ev.kind = COMP_WM_EVENT_CLICK;
                ev.client_id = COMP_WM_CLIENT_NONE;
                ev.surface_id = 0;
                ev.px = (int32_t)ms->x;
                ev.py = (int32_t)ms->y;
                ev.buttons = (uint32_t)ms->buttons;
                ev.flags = COMP_WM_EVENT_FLAG_BACKGROUND;
                if (wm_send_event(wm, &ev, 1) < 0) {
                    wm_disconnect(wm);
                }
            }
            if (!(wm && wm->connected)) {
                st->focus_client = -1;
                st->focus_surface_id = 0;
            }
        }
    }
}

static int comp_send_mouse(comp_client_t* clients, int nclients, comp_input_state_t* st, const mouse_state_t* ms) {
    if (!st || !ms) return 0;

    uint32_t mx = (uint32_t)ms->x;
    uint32_t my = (uint32_t)ms->y;
    uint32_t mb = (uint32_t)ms->buttons;

    if (st->wm_pointer_grab_active) {
        const int released = (mb == 0u) && (st->prev_buttons != 0u);
        if (released) {
            st->grab_active = 0;
            st->grab_client = -1;
            st->grab_surface_id = 0;
        }
        st->prev_buttons = mb;
        return 0;
    }

    int ci = -1;
    uint32_t sid = 0;
    comp_surface_t* s = 0;
    if (st->grab_active && st->grab_client >= 0 && st->grab_client < nclients && clients[st->grab_client].connected && comp_client_surface_id_valid(&clients[st->grab_client], st->grab_surface_id)) {
        ci = st->grab_client;
        sid = st->grab_surface_id;
        s = comp_client_surface_find(&clients[ci], sid);
    } else {
        (void)comp_pick_surface_at(clients, nclients, ms->x, ms->y, &ci, &sid, &s);
    }

    if ((ci < 0 || sid == 0 || !s) && (mb & 1u)) {
        int p_ci = -1;
        uint32_t p_sid = 0;
        comp_surface_t* p_s = 0;
        if (comp_pick_surface_at(clients, nclients, ms->x, ms->y, &p_ci, &p_sid, &p_s)) {
            st->grab_active = 1;
            st->grab_client = p_ci;
            st->grab_surface_id = p_sid;
            ci = p_ci;
            sid = p_sid;
            s = p_s;
        }
    }

    if (mx == st->last_mx && my == st->last_my && mb == st->last_mb && ci == st->last_client && sid == st->last_surface_id) {
        return 0;
    }

    st->last_mx = mx;
    st->last_my = my;
    st->last_mb = mb;
    st->last_client = ci;
    st->last_surface_id = sid;

    if (ci < 0 || sid == 0 || !s) {
        const int released = ((mb & 1u) == 0) && ((st->prev_buttons & 1u) != 0);
        if (released) {
            st->grab_active = 0;
            st->grab_client = -1;
            st->grab_surface_id = 0;
        }
        st->prev_buttons = mb;
        return 0;
    }

    comp_client_t* c = &clients[ci];
    if (!c->connected || c->fd_s2c < 0) return 0;

    comp_ipc_input_t in;
    in.surface_id = sid;
    in.kind = COMP_IPC_INPUT_MOUSE;
    {
        int32_t lx = (int32_t)(ms->x - s->x);
        int32_t ly = (int32_t)(ms->y - s->y);
        in.x = lx;
        in.y = ly;
    }
    in.buttons = (uint32_t)ms->buttons;
    in.keycode = 0;
    in.key_state = 0;

    comp_ipc_hdr_t hdr;
    hdr.magic = COMP_IPC_MAGIC;
    hdr.version = (uint16_t)COMP_IPC_VERSION;
    hdr.type = (uint16_t)COMP_IPC_MSG_INPUT;
    hdr.len = (uint32_t)sizeof(in);
    hdr.seq = c->seq_out++;

    uint8_t frame[sizeof(hdr) + sizeof(in)];
    memcpy(frame, &hdr, sizeof(hdr));
    memcpy(frame + sizeof(hdr), &in, sizeof(in));

    int wr = pipe_try_write_frame(c->fd_s2c, frame, (uint32_t)sizeof(frame), 0);
    if (wr < 0) {
        st->prev_buttons = mb;
        return -1;
    }

    const int released = ((mb & 1u) == 0) && ((st->prev_buttons & 1u) != 0);
    if (released) {
        st->grab_active = 0;
        st->grab_client = -1;
        st->grab_surface_id = 0;
    }
    st->prev_buttons = mb;
    return 0;
}

static int comp_send_key(comp_client_t* clients, int nclients, comp_input_state_t* st, uint32_t keycode, uint32_t key_state) {
    if (!st) return 0;
    if (st->focus_client < 0 || st->focus_client >= nclients) return 0;

    comp_client_t* c = &clients[st->focus_client];
    if (!c->connected || c->fd_s2c < 0) return 0;
    if (!comp_client_surface_id_valid(c, st->focus_surface_id)) return 0;

    comp_ipc_input_t in;
    in.surface_id = st->focus_surface_id;
    in.kind = COMP_IPC_INPUT_KEY;
    in.x = 0;
    in.y = 0;
    in.buttons = 0;
    in.keycode = keycode;
    in.key_state = key_state;

    comp_ipc_hdr_t hdr;
    hdr.magic = COMP_IPC_MAGIC;
    hdr.version = (uint16_t)COMP_IPC_VERSION;
    hdr.type = (uint16_t)COMP_IPC_MSG_INPUT;
    hdr.len = (uint32_t)sizeof(in);
    hdr.seq = c->seq_out++;

    uint8_t frame[sizeof(hdr) + sizeof(in)];
    memcpy(frame, &hdr, sizeof(hdr));
    memcpy(frame + sizeof(hdr), &in, sizeof(in));

    int wr = pipe_try_write_frame(c->fd_s2c, frame, (uint32_t)sizeof(frame), 1);
    if (wr < 0) return -1;
    return 0;
}

static void comp_client_pump(comp_client_t* c, const comp_buffer_t* buf, uint32_t* z_counter, wm_conn_t* wm, uint32_t client_id) {
    if (!c || !c->connected || c->fd_c2s < 0) return;
    if (!z_counter) return;

    int saw_eof = 0;

    for (;;) {
        uint8_t tmp[128];
        int rn = pipe_try_read(c->fd_c2s, tmp, (uint32_t)sizeof(tmp));
        if (rn < 0) {
            saw_eof = 1;
            break;
        }
        if (rn == 0) break;
        ipc_rx_push(&c->rx, tmp, (uint32_t)rn);
    }

    for (;;) {
        uint32_t avail = ipc_rx_count(&c->rx);
        if (avail < 4) break;

        uint32_t magic;
        ipc_rx_peek(&c->rx, 0, &magic, 4);
        if (magic != COMP_IPC_MAGIC) {
            ipc_rx_drop(&c->rx, 1);
            continue;
        }

        if (avail < (uint32_t)sizeof(comp_ipc_hdr_t)) break;

        comp_ipc_hdr_t hdr;
        ipc_rx_peek(&c->rx, 0, &hdr, (uint32_t)sizeof(hdr));

        if (hdr.version != COMP_IPC_VERSION) {
            ipc_rx_drop(&c->rx, 1);
            continue;
        }
        if (hdr.len > COMP_IPC_MAX_PAYLOAD) {
            ipc_rx_drop(&c->rx, 1);
            continue;
        }

        uint32_t frame_len = (uint32_t)sizeof(hdr) + hdr.len;
        if (avail < frame_len) break;

        ipc_rx_drop(&c->rx, (uint32_t)sizeof(hdr));
        uint8_t payload[COMP_IPC_MAX_PAYLOAD];
        if (hdr.len) {
            ipc_rx_peek(&c->rx, 0, payload, hdr.len);
            ipc_rx_drop(&c->rx, hdr.len);
        }

        g_dbg_curr_pid = c->pid;
        g_dbg_last_rx_pid = c->pid;
        g_dbg_last_rx_type = (uint16_t)hdr.type;
        g_dbg_last_rx_seq = hdr.seq;
        g_dbg_last_rx_surface_id = 0;

        if (hdr.type == COMP_IPC_MSG_HELLO && hdr.len == (uint32_t)sizeof(comp_ipc_hello_t)) {
            comp_ipc_hello_t h;
            memcpy(&h, payload, sizeof(h));
            c->pid = (int)h.client_pid;
            comp_send_ack(c->fd_s2c, hdr.seq, (uint16_t)hdr.type, 0u, 0u);
        } else if (hdr.type == COMP_IPC_MSG_ATTACH_SHM && hdr.len == (uint32_t)sizeof(comp_ipc_attach_shm_t)) {
            comp_ipc_attach_shm_t a;
            memcpy(&a, payload, sizeof(a));

            g_dbg_last_rx_surface_id = a.surface_id;

            comp_surface_t* s = comp_client_surface_get(c, a.surface_id, 1);
            if (!s) {
                comp_send_error(c->fd_s2c, hdr.seq, (uint16_t)hdr.type, (uint16_t)COMP_IPC_ERR_INVALID, a.surface_id, 0);
                continue;
            }

            if (!(buf && buf->pixels && buf->shm_fd >= 0 && (int)a.shm_fd == buf->shm_fd)) {
                comp_send_error(c->fd_s2c, hdr.seq, (uint16_t)hdr.type, (uint16_t)COMP_IPC_ERR_INVALID, a.surface_id, 0);
                continue;
            }

            {
                if (s->owns_buffer) {
                    if (s->pixels && s->size_bytes) munmap((void*)s->pixels, s->size_bytes);
                    if (s->shm_fd >= 0) close(s->shm_fd);
                    s->owns_buffer = 0;
                    s->shm_fd = -1;
                    s->size_bytes = 0;
                }
                s->attached = 1;
                s->pixels = buf->pixels;
                s->w = (int)a.width;
                s->h = (int)a.height;
                s->stride = (int)a.stride;
                if (s->stride <= 0) s->stride = s->w;
            }
            comp_send_ack(c->fd_s2c, hdr.seq, (uint16_t)hdr.type, a.surface_id, 0);
        } else if (hdr.type == COMP_IPC_MSG_ATTACH_SHM_NAME && hdr.len == (uint32_t)sizeof(comp_ipc_attach_shm_name_t)) {
            comp_ipc_attach_shm_name_t a;
            memcpy(&a, payload, sizeof(a));

            g_dbg_last_rx_surface_id = a.surface_id;
            if (a.surface_id == 0x80000001u) {
                g_dbg_bar_rx_pid = c->pid;
                g_dbg_bar_rx_type = (uint16_t)hdr.type;
                g_dbg_bar_rx_seq = hdr.seq;
            }

            comp_surface_t* s = comp_client_surface_get(c, a.surface_id, 1);
            if (!s) {
                comp_send_error(c->fd_s2c, hdr.seq, (uint16_t)hdr.type, (uint16_t)COMP_IPC_ERR_INVALID, a.surface_id, 0);
                continue;
            }

            if (a.width == 0 || a.height == 0) {
                comp_send_error(c->fd_s2c, hdr.seq, (uint16_t)hdr.type, (uint16_t)COMP_IPC_ERR_INVALID, a.surface_id, 0);
                continue;
            }
            if (a.stride == 0) a.stride = a.width;
            if (a.stride < a.width) {
                comp_send_error(c->fd_s2c, hdr.seq, (uint16_t)hdr.type, (uint16_t)COMP_IPC_ERR_INVALID, a.surface_id, 0);
                continue;
            }

            uint64_t min_size = (uint64_t)a.height * (uint64_t)a.stride * 4ull;
            if (min_size == 0) {
                comp_send_error(c->fd_s2c, hdr.seq, (uint16_t)hdr.type, (uint16_t)COMP_IPC_ERR_INVALID, a.surface_id, 0);
                continue;
            }
            if (a.size_bytes < (uint32_t)min_size) {
                comp_send_error(c->fd_s2c, hdr.seq, (uint16_t)hdr.type, (uint16_t)COMP_IPC_ERR_INVALID, a.surface_id, 0);
                continue;
            }
            if (a.size_bytes > (64u * 1024u * 1024u)) {
                comp_send_error(c->fd_s2c, hdr.seq, (uint16_t)hdr.type, (uint16_t)COMP_IPC_ERR_INVALID, a.surface_id, 0);
                continue;
            }

            char name[32];
            memcpy(name, a.shm_name, sizeof(name));
            name[sizeof(name) - 1u] = '\0';
            if (name[0] == '\0') {
                comp_send_error(c->fd_s2c, hdr.seq, (uint16_t)hdr.type, (uint16_t)COMP_IPC_ERR_INVALID, a.surface_id, 0);
                continue;
            }

            if (s->owns_buffer && s->pixels && s->shm_fd >= 0 && s->size_bytes >= a.size_bytes && memcmp(s->shm_name, name, sizeof(s->shm_name)) == 0) {
                s->attached = 1;
                s->committed = 0;
                s->w = (int)a.width;
                s->h = (int)a.height;
                s->stride = (int)a.stride;
                comp_send_ack(c->fd_s2c, hdr.seq, (uint16_t)hdr.type, a.surface_id, 0);
                continue;
            }

            int shm_fd = shm_open_named(name);
            if (shm_fd < 0) {
                comp_send_error(c->fd_s2c, hdr.seq, (uint16_t)hdr.type, (uint16_t)COMP_IPC_ERR_SHM_OPEN, a.surface_id, 0);
                continue;
            }

            uint32_t* pixels = (uint32_t*)mmap(shm_fd, a.size_bytes, MAP_SHARED);
            if (!pixels) {
                close(shm_fd);
                comp_send_error(c->fd_s2c, hdr.seq, (uint16_t)hdr.type, (uint16_t)COMP_IPC_ERR_SHM_MAP, a.surface_id, 0);
                continue;
            }

            if (s->owns_buffer) {
                if (s->pixels && s->size_bytes) munmap((void*)s->pixels, s->size_bytes);
                if (s->shm_fd >= 0) close(s->shm_fd);
            }

            s->attached = 1;
            s->committed = 0;
            s->pixels = pixels;
            s->w = (int)a.width;
            s->h = (int)a.height;
            s->stride = (int)a.stride;
            s->owns_buffer = 1;
            s->shm_fd = shm_fd;
            s->size_bytes = a.size_bytes;
            memcpy(s->shm_name, name, sizeof(s->shm_name));
            comp_send_ack(c->fd_s2c, hdr.seq, (uint16_t)hdr.type, a.surface_id, 0);
        } else if (hdr.type == COMP_IPC_MSG_COMMIT && hdr.len == (uint32_t)sizeof(comp_ipc_commit_t)) {
            comp_ipc_commit_t cm;
            memcpy(&cm, payload, sizeof(cm));

            g_dbg_last_rx_surface_id = cm.surface_id;
            if (cm.surface_id == 0x80000001u) {
                g_dbg_bar_rx_pid = c->pid;
                g_dbg_bar_rx_type = (uint16_t)hdr.type;
                g_dbg_bar_rx_seq = hdr.seq;
            }

            comp_surface_t* s = comp_client_surface_get(c, cm.surface_id, 0);
            if (!(s && s->attached)) {
                comp_send_error(c->fd_s2c, hdr.seq, (uint16_t)hdr.type, (uint16_t)COMP_IPC_ERR_NO_SURFACE, cm.surface_id, 0);
                continue;
            }
            {
                const int first_commit = (s->commit_gen == 0);
                const int was_committed = (s->committed != 0);

                if (cm.surface_id == 0x80000001u) {
                    s->x = 0;
                    s->y = 0;
                } else if (!(wm && wm->connected)) {
                    s->x = (int)cm.x;
                    s->y = (int)cm.y;
                }
                s->committed = 1;
                s->commit_gen = g_commit_gen++;

                if (cm.surface_id == 0x80000001u) {
                    s->z = ++(*z_counter);
                } else if (!(wm && wm->connected)) {
                    if (first_commit || (cm.flags & COMP_IPC_COMMIT_FLAG_RAISE)) {
                        s->z = ++(*z_counter);
                    }
                }

                if (wm && wm->connected) {
                    const int send = first_commit || (!was_committed);
                    if (send) {
                        comp_ipc_wm_event_t ev;
                        memset(&ev, 0, sizeof(ev));
                        ev.kind = first_commit ? COMP_WM_EVENT_MAP : COMP_WM_EVENT_COMMIT;
                        ev.client_id = client_id;
                        ev.surface_id = cm.surface_id;
                        ev.sx = (int32_t)s->x;
                        ev.sy = (int32_t)s->y;
                        ev.sw = (uint32_t)s->w;
                        ev.sh = (uint32_t)s->h;
                        ev.flags = 0;
                        if (wm_send_event(wm, &ev, first_commit ? 1 : 0) < 0) {
                            wm_disconnect(wm);
                        }
                    }
                }
            }
            if (cm.flags & COMP_IPC_COMMIT_FLAG_ACK) {
                comp_send_ack(c->fd_s2c, hdr.seq, (uint16_t)hdr.type, cm.surface_id, 0);
            }
        } else if (hdr.type == COMP_IPC_MSG_DESTROY_SURFACE && hdr.len == (uint32_t)sizeof(comp_ipc_destroy_surface_t)) {
            comp_ipc_destroy_surface_t d;
            memcpy(&d, payload, sizeof(d));

            g_dbg_last_rx_surface_id = d.surface_id;

            comp_surface_t* s = comp_client_surface_get(c, d.surface_id, 0);
            if (!s) {
                comp_send_error(c->fd_s2c, hdr.seq, (uint16_t)hdr.type, (uint16_t)COMP_IPC_ERR_NO_SURFACE, d.surface_id, 0);
                continue;
            }

            if (s->owns_buffer) {
                if (s->pixels && s->size_bytes) munmap((void*)s->pixels, s->size_bytes);
                if (s->shm_fd >= 0) close(s->shm_fd);
            }
            memset(s, 0, sizeof(*s));
            s->shm_fd = -1;
            comp_send_ack(c->fd_s2c, hdr.seq, (uint16_t)hdr.type, d.surface_id, 0);

            if (wm && wm->connected) {
                comp_ipc_wm_event_t ev;
                memset(&ev, 0, sizeof(ev));
                ev.kind = COMP_WM_EVENT_UNMAP;
                ev.client_id = client_id;
                ev.surface_id = d.surface_id;
                ev.flags = 0;
                if (wm_send_event(wm, &ev, 1) < 0) {
                    wm_disconnect(wm);
                }
            }
        }
    }

    if (saw_eof) {
        if (wm && wm->connected) {
            for (int si = 0; si < COMP_MAX_SURFACES; si++) {
                comp_surface_t* s = &c->surfaces[si];
                if (!s->in_use) continue;
                comp_ipc_wm_event_t ev;
                memset(&ev, 0, sizeof(ev));
                ev.kind = COMP_WM_EVENT_UNMAP;
                ev.client_id = client_id;
                ev.surface_id = s->id;
                ev.flags = 0;
                if (wm_send_event(wm, &ev, 1) < 0) {
                    wm_disconnect(wm);
                    break;
                }
            }
        }
        comp_client_disconnect(c);
    }
}

__attribute__((force_align_arg_pointer)) int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    dbg_write("compositor: enter main\n");

    dbg_write("compositor: install signals\n");
    signal(2, (void*)on_signal);
    signal(15, (void*)on_signal);
    dbg_write("compositor: signals ok\n");

    dbg_write("compositor: open /dev/fb0\n");
    int fd_fb = open("/dev/fb0", 0);
    if (fd_fb < 0) {
        dbg_write("compositor: cannot open /dev/fb0\n");
        return 1;
    }

    dbg_write("compositor: read fb info\n");
    fb_info_t info;
    int r = read(fd_fb, &info, sizeof(info));
    close(fd_fb);
    dbg_write("compositor: fb info read done\n");

    if (r < (int)sizeof(info) || info.width == 0 || info.height == 0 || info.pitch == 0) {
        dbg_write("compositor: bad fb info\n");
        return 1;
    }

    dbg_write("compositor: open /dev/mouse\n");
    int fd_mouse = open("/dev/mouse", 0);
    if (fd_mouse < 0) {
        dbg_write("compositor: open mouse failed\n");
        return 1;
    }

    int listen_fd = -1;
    int wm_listen_fd = -1;

    dbg_write("compositor: fb_acquire\n");
    if (fb_acquire() != 0) {
        dbg_write("compositor: fb busy\n");
        close(fd_mouse);
        return 1;
    }
    dbg_write("compositor: fb acquired\n");

    dbg_write("compositor: map_framebuffer\n");
    uint32_t* fb = (uint32_t*)map_framebuffer();
    if (!fb) {
        close(fd_mouse);
        fb_release();
        g_fb_released = 1;
        dbg_write("compositor: map_framebuffer failed\n");
        return 1;
    }
    dbg_write("compositor: fb mapped\n");

    int w = (int)info.width;
    int h = (int)info.height;
    int stride = (int)(info.pitch / 4u);
    if (stride <= 0) stride = w;

    int frame_shm_fd = -1;
    uint32_t* frame_pixels = 0;
    uint32_t frame_size_bytes = 0;
    {
        uint64_t fb_bytes64 = (uint64_t)info.pitch * (uint64_t)info.height;
        if (fb_bytes64 > 0 && fb_bytes64 <= 0xFFFFFFFFu) {
            frame_size_bytes = (uint32_t)fb_bytes64;
            frame_shm_fd = shm_create(frame_size_bytes);
            if (frame_shm_fd >= 0) {
                frame_pixels = (uint32_t*)mmap(frame_shm_fd, frame_size_bytes, MAP_SHARED);
                if (!frame_pixels) {
                    close(frame_shm_fd);
                    frame_shm_fd = -1;
                }
            }
        }
    }

    int shm_w = 320;
    int shm_h = 240;
    uint32_t shm_size = (uint32_t)shm_w * (uint32_t)shm_h * 4u;
    int shm_fd = shm_create(shm_size);
    if (shm_fd < 0) {
        dbg_write("compositor: shm_create failed\n");
    }

    comp_buffer_t buf;
    memset(&buf, 0, sizeof(buf));
    buf.shm_fd = shm_fd;
    buf.size_bytes = shm_size;
    buf.w = shm_w;
    buf.h = shm_h;
    buf.stride = shm_w;
    buf.pixels = 0;
    if (buf.shm_fd >= 0) {
        buf.pixels = (uint32_t*)mmap(buf.shm_fd, buf.size_bytes, MAP_SHARED);
        if (!buf.pixels) {
            dbg_write("compositor: mmap(shm) failed\n");
        }
    }

    int ipc_fds[2] = { -1, -1 };
    int ipc_back[2] = { -1, -1 };
    int have_ipc = 0;
    int child_pid = -1;
    if (buf.shm_fd >= 0 && buf.pixels && pipe(ipc_fds) == 0 && pipe(ipc_back) == 0) {
        char shm_s[16];
        char w_s[16];
        char h_s[16];
        char c2s_w_s[16];
        char s2c_r_s[16];
        char c2s_r_s[16];
        char s2c_w_s[16];

        itoa(buf.shm_fd, shm_s, 10);
        itoa(shm_w, w_s, 10);
        itoa(shm_h, h_s, 10);
        itoa(ipc_fds[1], c2s_w_s, 10);
        itoa(ipc_back[0], s2c_r_s, 10);
        itoa(ipc_fds[0], c2s_r_s, 10);
        itoa(ipc_back[1], s2c_w_s, 10);

        char* argv[8];
        argv[0] = (char*)"comp_client";
        argv[1] = shm_s;
        argv[2] = w_s;
        argv[3] = h_s;
        argv[4] = c2s_w_s;
        argv[5] = s2c_r_s;
        argv[6] = c2s_r_s;
        argv[7] = s2c_w_s;

        child_pid = spawn_process("/bin/comp_client.exe", 8, argv);
        if (child_pid >= 0) {
            have_ipc = 1;
            close(ipc_fds[1]);
            close(ipc_back[0]);
        } else {
            dbg_write("compositor: spawn comp_client failed\n");
            close(ipc_fds[0]);
            close(ipc_fds[1]);
            close(ipc_back[0]);
            close(ipc_back[1]);
            ipc_fds[0] = -1;
            ipc_fds[1] = -1;
            ipc_back[0] = -1;
            ipc_back[1] = -1;
        }
    } else {
        if (ipc_fds[0] >= 0) close(ipc_fds[0]);
        if (ipc_fds[1] >= 0) close(ipc_fds[1]);
        if (ipc_back[0] >= 0) close(ipc_back[0]);
        if (ipc_back[1] >= 0) close(ipc_back[1]);
        ipc_fds[0] = -1;
        ipc_fds[1] = -1;
        ipc_back[0] = -1;
        ipc_back[1] = -1;
    }

    comp_client_t clients[COMP_MAX_CLIENTS];
    memset(clients, 0, sizeof(clients));

    if (have_ipc) {
        comp_client_init(&clients[0], child_pid, ipc_fds[0], ipc_back[1]);
    } else {
        clients[0].connected = 0;
        clients[0].pid = -1;
        clients[0].fd_c2s = -1;
        clients[0].fd_s2c = -1;
    }

    comp_input_state_t input;
    comp_input_state_init(&input);

    uint32_t z_counter = 1;

    listen_fd = ipc_listen("compositor");
    if (listen_fd < 0) {
        dbg_write("compositor: ipc_listen failed\n");
    }

    wm_conn_t wm;
    memset(&wm, 0, sizeof(wm));
    wm.fd_c2s = -1;
    wm.fd_s2c = -1;
    wm.connected = 0;
    ipc_rx_reset(&wm.rx);
    wm.seq_out = 1;

    wm_listen_fd = ipc_listen("compositor_wm");
    if (wm_listen_fd < 0) {
        dbg_write("compositor: ipc_listen compositor_wm failed\n");
    }

    int wm_pid = -1;
    int wm_spawn_cooldown = 0;
    int wm_spawn_retry_wait = 0;

    mouse_state_t ms_last;
    ms_last.x = w / 2;
    ms_last.y = h / 2;
    ms_last.buttons = 0;

    int32_t draw_mx = 0x7FFFFFFF;
    int32_t draw_my = 0x7FFFFFFF;
    int prev_focus_client = -2;
    uint32_t prev_focus_sid = 0xFFFFFFFFu;
    uint64_t prev_sigs[COMP_MAX_CLIENTS * COMP_MAX_SURFACES];
    for (int i = 0; i < (int)(sizeof(prev_sigs) / sizeof(prev_sigs[0])); i++) prev_sigs[i] = 0;

    comp_preview_t preview;
    memset(&preview, 0, sizeof(preview));
    int preview_dirty = 0;

    while (!g_should_exit) {
        if (wm_spawn_retry_wait > 0) wm_spawn_retry_wait--;
        if (!wm.connected && wm_pid > 0) {
            if (wm_spawn_cooldown > 0) {
                wm_spawn_cooldown--;
            } else {
                wm_pid = -1;
            }
        }

        if (wm_listen_fd < 0) {
            wm_listen_fd = ipc_listen("compositor_wm");
        }
        if (!wm.connected && wm_listen_fd >= 0) {
            int fds[2] = { -1, -1 };
            int ar = ipc_accept(wm_listen_fd, fds);
            if (ar == 1) {
                wm_init(&wm, fds[0], fds[1]);
                if (wm_pid < 0) {
                    wm_pid = 0;
                }
                wm_replay_state(&wm, clients, COMP_MAX_CLIENTS);
            }
        }

        if (!wm.connected && wm_pid < 0 && wm_spawn_retry_wait == 0 && listen_fd >= 0 && wm_listen_fd >= 0) {
            char* wargv[1];
            wargv[0] = (char*)"wm";
            wm_pid = spawn_process("/bin/wm.exe", 1, wargv);
            if (wm_pid < 0) {
                dbg_write("compositor: spawn wm failed\n");
                wm_spawn_retry_wait = 200;
            } else {
                wm_spawn_cooldown = 200;
            }
        }

        if (wm.connected) {
            wm_pump(&wm, clients, COMP_MAX_CLIENTS, &input, &z_counter, &preview, &preview_dirty);
            if (!wm.connected) {
                input.focus_client = -1;
                input.focus_surface_id = 0;
                input.wm_pointer_grab_active = 0;
                input.wm_pointer_grab_client = -1;
                input.wm_pointer_grab_surface_id = 0;
                if (preview.active) {
                    preview.active = 0;
                    preview_dirty = 1;
                }
            }
        }

        if (listen_fd >= 0) {
            for (;;) {
                int fds[2] = { -1, -1 };
                int ar = ipc_accept(listen_fd, fds);
                if (ar != 1) break;

                int slot = -1;
                for (int i = 0; i < COMP_MAX_CLIENTS; i++) {
                    if (!clients[i].connected) {
                        slot = i;
                        break;
                    }
                }

                if (slot >= 0) {
                    comp_client_init(&clients[slot], -1, fds[0], fds[1]);
                    dbg_write("compositor: accepted client\n");
                } else {
                    dbg_write("compositor: reject client (no slots)\n");
                    if (fds[0] >= 0) close(fds[0]);
                    if (fds[1] >= 0) close(fds[1]);
                }
            }
        }

        for (int ci = 0; ci < COMP_MAX_CLIENTS; ci++) {
            if (!clients[ci].connected) continue;
            comp_client_pump(&clients[ci], &buf, &z_counter, &wm, (uint32_t)ci);
        }

        if (wm.connected) {
            wm_pump(&wm, clients, COMP_MAX_CLIENTS, &input, &z_counter, &preview, &preview_dirty);
        }

        mouse_state_t ms;
        int mr = read(fd_mouse, &ms, sizeof(ms));
        if (mr < (int)sizeof(ms)) {
            ms = ms_last;
        } else {
            ms_last = ms;
        }

        comp_update_focus(clients, COMP_MAX_CLIENTS, &input, &ms, &z_counter, &wm);

        if (wm.connected) {
            comp_send_wm_pointer(&wm, clients, COMP_MAX_CLIENTS, &input, &ms);
            if (wm.connected) {
                wm_pump(&wm, clients, COMP_MAX_CLIENTS, &input, &z_counter, &preview, &preview_dirty);
            }
            if (!wm.connected) {
                input.focus_client = -1;
                input.focus_surface_id = 0;
                input.wm_pointer_grab_active = 0;
                input.wm_pointer_grab_client = -1;
                input.wm_pointer_grab_surface_id = 0;
                if (preview.active) {
                    preview.active = 0;
                    preview_dirty = 1;
                }
            }
        }

        if (comp_send_mouse(clients, COMP_MAX_CLIENTS, &input, &ms) < 0) {
            int dc = input.last_client;
            if (dc >= 0 && dc < COMP_MAX_CLIENTS && clients[dc].connected) {
                dbg_write("compositor: client disconnected\n");
                if (wm.connected) {
                    for (int si = 0; si < COMP_MAX_SURFACES; si++) {
                        comp_surface_t* s = &clients[dc].surfaces[si];
                        if (!s->in_use) continue;
                        comp_ipc_wm_event_t ev;
                        memset(&ev, 0, sizeof(ev));
                        ev.kind = COMP_WM_EVENT_UNMAP;
                        ev.client_id = (uint32_t)dc;
                        ev.surface_id = s->id;
                        ev.flags = 0;
                        if (wm_send_event(&wm, &ev, 1) < 0) {
                            wm_disconnect(&wm);
                            input.focus_client = -1;
                            input.focus_surface_id = 0;
                            break;
                        }
                    }
                }
                comp_client_disconnect(&clients[dc]);
            }
        }

        for (;;) {
            char kc = 0;
            int kr = kbd_try_read(&kc);
            if (kr <= 0) break;

            if (wm.connected) {
                comp_ipc_wm_event_t ev;
                memset(&ev, 0, sizeof(ev));
                ev.kind = COMP_WM_EVENT_KEY;
                ev.client_id = input.focus_client >= 0 ? (uint32_t)input.focus_client : COMP_WM_CLIENT_NONE;
                ev.surface_id = input.focus_surface_id;
                ev.keycode = (uint32_t)(uint8_t)kc;
                ev.key_state = 1u;

                if (input.focus_client >= 0 && input.focus_client < COMP_MAX_CLIENTS) {
                    comp_client_t* c = &clients[input.focus_client];
                    comp_surface_t* s = comp_client_surface_find(c, input.focus_surface_id);
                    if (s && s->attached && s->committed) {
                        ev.sx = (int32_t)s->x;
                        ev.sy = (int32_t)s->y;
                        ev.sw = (uint32_t)s->w;
                        ev.sh = (uint32_t)s->h;
                    }
                }

                if (wm_send_event(&wm, &ev, 1) < 0) {
                    wm_disconnect(&wm);
                    input.focus_client = -1;
                    input.focus_surface_id = 0;
                }
            }

            if (comp_send_key(clients, COMP_MAX_CLIENTS, &input, (uint32_t)(uint8_t)kc, 1u) < 0) {
                int dc = input.focus_client;
                if (dc >= 0 && dc < COMP_MAX_CLIENTS && clients[dc].connected) {
                    dbg_write("compositor: client disconnected\n");
                    if (wm.connected) {
                        for (int si = 0; si < COMP_MAX_SURFACES; si++) {
                            comp_surface_t* s = &clients[dc].surfaces[si];
                            if (!s->in_use) continue;
                            comp_ipc_wm_event_t ev;
                            memset(&ev, 0, sizeof(ev));
                            ev.kind = COMP_WM_EVENT_UNMAP;
                            ev.client_id = (uint32_t)dc;
                            ev.surface_id = s->id;
                            ev.flags = 0;
                            if (wm_send_event(&wm, &ev, 1) < 0) {
                                wm_disconnect(&wm);
                                input.focus_client = -1;
                                input.focus_surface_id = 0;
                                break;
                            }
                        }
                    }
                    comp_client_disconnect(&clients[dc]);
                }
                break;
            }
        }

        int need_redraw = 0;
        if (ms.x != draw_mx || ms.y != draw_my) need_redraw = 1;
        if (input.focus_client != prev_focus_client || input.focus_surface_id != prev_focus_sid) need_redraw = 1;
        if (preview_dirty) need_redraw = 1;

        for (int ci = 0; ci < COMP_MAX_CLIENTS; ci++) {
            for (int si = 0; si < COMP_MAX_SURFACES; si++) {
                const comp_surface_t* s = &clients[ci].surfaces[si];
                const int valid = (clients[ci].connected && s->in_use && s->attached && s->committed && s->pixels && s->w > 0 && s->h > 0 && s->stride > 0);

                uint64_t sig = 0;
                if (valid) {
                    uint64_t h1 = (uint64_t)(uint32_t)s->x;
                    uint64_t h2 = (uint64_t)(uint32_t)s->y;
                    uint64_t h3 = (uint64_t)(uint32_t)s->w;
                    uint64_t h4 = (uint64_t)(uint32_t)s->h;
                    uint64_t h5 = (uint64_t)(uint32_t)s->stride;
                    uint64_t h6 = (uint64_t)(uint32_t)s->z;
                    uint64_t h7 = (uint64_t)(uintptr_t)s->pixels;
                    uint64_t h8 = (uint64_t)s->commit_gen;
                    sig = h1;
                    sig ^= (h2 * 0x9E3779B97F4A7C15ull);
                    sig ^= (h3 * 0xC2B2AE3D27D4EB4Full);
                    sig ^= (h4 * 0x165667B19E3779F9ull);
                    sig ^= (h5 * 0x85EBCA77C2B2AE63ull);
                    sig ^= (h6 * 0x27D4EB2F165667C5ull);
                    sig ^= (h7 * 0x9E3779B97F4A7C15ull);
                    sig ^= (h8 * 0xC2B2AE3D27D4EB4Full);
                }

                const int idx = ci * COMP_MAX_SURFACES + si;
                if (prev_sigs[idx] != sig) need_redraw = 1;
                prev_sigs[idx] = sig;
            }
        }

        if (need_redraw) {
            preview_dirty = 0;
            draw_mx = ms.x;
            draw_my = ms.y;
            prev_focus_client = input.focus_client;
            prev_focus_sid = input.focus_surface_id;

            uint32_t bg = 0x101010;
            uint32_t* out = frame_pixels ? frame_pixels : fb;
            fill_rect(out, stride, w, h, 0, 0, w, h, bg);

            struct draw_item {
                uint32_t z;
                int ci;
                int si;
            };
            struct draw_item order[COMP_MAX_CLIENTS * COMP_MAX_SURFACES];
            int order_n = 0;
            for (int ci = 0; ci < COMP_MAX_CLIENTS; ci++) {
                if (!clients[ci].connected) continue;
                for (int si = 0; si < COMP_MAX_SURFACES; si++) {
                    comp_surface_t* s = &clients[ci].surfaces[si];
                    if (!s->in_use || !s->attached || !s->committed) continue;
                    if (!s->pixels || s->w <= 0 || s->h <= 0 || s->stride <= 0) continue;
                    order[order_n].z = s->z;
                    order[order_n].ci = ci;
                    order[order_n].si = si;
                    order_n++;
                }
            }
            for (int i = 0; i < order_n; i++) {
                for (int j = i + 1; j < order_n; j++) {
                    if (order[i].z > order[j].z) {
                        struct draw_item tmp = order[i];
                        order[i] = order[j];
                        order[j] = tmp;
                    }
                }
            }
            for (int k = 0; k < order_n; k++) {
                comp_surface_t* s = &clients[order[k].ci].surfaces[order[k].si];
                blit_surface(out, stride, w, h, s->x, s->y, s->pixels, s->stride, s->w, s->h);
            }

            if (preview.active && preview.client_id < (uint32_t)COMP_MAX_CLIENTS) {
                comp_client_t* pc = &clients[(int)preview.client_id];
                if (pc->connected) {
                    comp_surface_t* ps = comp_client_surface_find(pc, preview.surface_id);
                    if (ps && ps->in_use && ps->attached && ps->committed) {
                        const int t = 2;
                        const uint32_t col = 0x007ACC;
                        draw_frame_rect(out, stride, w, h, ps->x - t, ps->y - t, (int)preview.w + t * 2, (int)preview.h + t * 2, t, col);
                    } else {
                        preview.active = 0;
                    }
                } else {
                    preview.active = 0;
                }
            }

            draw_cursor(out, stride, w, h, ms.x, ms.y);

            if (frame_pixels) {
                memcpy((void*)fb, (const void*)frame_pixels, (size_t)frame_size_bytes);
            }
        }

        usleep(16000);
    }

    close(fd_mouse);

    if (frame_pixels && frame_size_bytes) {
        munmap((void*)frame_pixels, frame_size_bytes);
        frame_pixels = 0;
    }
    if (frame_shm_fd >= 0) {
        close(frame_shm_fd);
        frame_shm_fd = -1;
    }

    for (int i = 0; i < COMP_MAX_CLIENTS; i++) {
        if (clients[i].connected) {
            comp_client_disconnect(&clients[i]);
        }
    }

    if (wm_pid > 0) {
        (void)syscall(9, wm_pid, 0, 0);
        wm_pid = -1;
    }

    comp_buffer_destroy(&buf);

    if (!g_fb_released) {
        fb_release();
        g_fb_released = 1;
    }

    return 0;
}
