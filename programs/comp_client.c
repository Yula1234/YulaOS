// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#include <yula.h>
#include <comp.h>

#define COMP_CLIENT_RX_CAP 2048u
#define COMP_CLIENT_RX_MASK (COMP_CLIENT_RX_CAP - 1u)

typedef struct {
    uint8_t buf[COMP_CLIENT_RX_CAP];
    uint32_t r;
    uint32_t w;
} rx_ring_t;

static volatile int g_should_exit = 0;

static void on_signal(int sig) {
    (void)sig;
    g_should_exit = 1;
}

static inline uint32_t rx_count(const rx_ring_t* q) {
    return q->w - q->r;
}

static inline void rx_drop(rx_ring_t* q, uint32_t n) {
    uint32_t c = rx_count(q);
    if (n > c) n = c;
    q->r += n;
}

static inline void rx_peek(const rx_ring_t* q, uint32_t off, void* dst, uint32_t n) {
    uint8_t* out = (uint8_t*)dst;
    uint32_t ri = (q->r + off) & COMP_CLIENT_RX_MASK;
    uint32_t first = COMP_CLIENT_RX_CAP - ri;
    if (first > n) first = n;
    memcpy(out, &q->buf[ri], first);
    if (n > first) memcpy(out + first, &q->buf[0], n - first);
}

static inline void rx_push(rx_ring_t* q, const uint8_t* src, uint32_t n) {
    if (!src || n == 0) return;
    uint32_t count = rx_count(q);
    if (n > COMP_CLIENT_RX_CAP) {
        src += (n - COMP_CLIENT_RX_CAP);
        n = COMP_CLIENT_RX_CAP;
        q->r = 0;
        q->w = 0;
        count = 0;
    }
    if (count + n > COMP_CLIENT_RX_CAP) {
        uint32_t drop = (count + n) - COMP_CLIENT_RX_CAP;
        q->r += drop;
    }

    uint32_t wi = q->w & COMP_CLIENT_RX_MASK;
    uint32_t first = COMP_CLIENT_RX_CAP - wi;
    if (first > n) first = n;
    memcpy(&q->buf[wi], src, first);
    if (n > first) memcpy(&q->buf[0], src + first, n - first);
    q->w += n;
}

__attribute__((force_align_arg_pointer)) int main(int argc, char** argv) {
    int width = 320;
    int height = 240;
    int legacy_mode = 0;

    signal(2, (void*)on_signal);
    signal(15, (void*)on_signal);

    int shm_fd = -1;
    int c2s_w_fd = -1;
    int s2c_r_fd = -1;

    char shm_name[32];
    shm_name[0] = '\0';
    int have_shm_name = 0;

    comp_conn_t conn;
    comp_conn_reset(&conn);

    if (argc >= 8) {
        legacy_mode = 1;

        shm_fd = atoi(argv[1]);
        width = atoi(argv[2]);
        height = atoi(argv[3]);
        c2s_w_fd = atoi(argv[4]);
        s2c_r_fd = atoi(argv[5]);
        int c2s_r_fd = atoi(argv[6]);
        int s2c_w_fd = atoi(argv[7]);

        if (shm_fd < 0 || c2s_w_fd < 0 || s2c_r_fd < 0 || width <= 0 || height <= 0) {
            return 1;
        }

        if (c2s_r_fd >= 0) close(c2s_r_fd);
        if (s2c_w_fd >= 0) close(s2c_w_fd);
    } else {
        if (argc >= 3) {
            width = atoi(argv[1]);
            height = atoi(argv[2]);
            if (width <= 0) width = 320;
            if (height <= 0) height = 240;
        }

        const int pid = getpid();
        int created = 0;
        for (int i = 0; i < 8; i++) {
            (void)snprintf(shm_name, sizeof(shm_name), "cc_%d_%d", pid, i);
            shm_fd = shm_create_named(shm_name, (uint32_t)width * (uint32_t)height * 4u);
            if (shm_fd >= 0) {
                created = 1;
                have_shm_name = 1;
                if (comp_connect(&conn, "flux") != 0) {
                    close(shm_fd);
                    shm_fd = -1;
                    shm_unlink_named(shm_name);
                    return 1;
                }

                if (comp_send_hello(&conn) != 0) {
                    comp_disconnect(&conn);
                    close(shm_fd);
                    shm_unlink_named(shm_name);
                    return 1;
                }

                uint32_t size_bytes = (uint32_t)width * (uint32_t)height * 4u;
                if (comp_send_attach_shm_name(&conn, 1u, shm_name, size_bytes, (uint32_t)width, (uint32_t)height, (uint32_t)width, 0u) != 0) {
                    comp_disconnect(&conn);
                    close(shm_fd);
                    shm_unlink_named(shm_name);
                    return 1;
                }

                int init_x = 16 + (pid % 5) * 32;
                int init_y = 16 + (pid % 7) * 24;
                if (comp_send_commit(&conn, 1u, init_x, init_y, 0u) != 0) {
                    comp_disconnect(&conn);
                    close(shm_fd);
                    shm_unlink_named(shm_name);
                    return 1;
                }

                break;
            }
        }
        if (!created) {
            return 1;
        }

        c2s_w_fd = conn.fd_c2s_w;
        s2c_r_fd = conn.fd_s2c_r;
    }

    uint32_t stride = (uint32_t)width;
    uint32_t size_bytes = (uint32_t)width * (uint32_t)height * 4u;

    uint32_t* pixels = (uint32_t*)mmap(shm_fd, size_bytes, MAP_SHARED);
    if (!pixels) {
        if (!legacy_mode) {
            comp_disconnect(&conn);
            if (shm_fd >= 0) close(shm_fd);
            if (have_shm_name) shm_unlink_named(shm_name);
        }
        return 1;
    }

    uint32_t seq = 1;
    comp_ipc_commit_t commit;
    commit.surface_id = 1;
    commit.x = 16;
    commit.y = 16;
    commit.flags = 0;

    rx_ring_t rx;
    rx.r = 0;
    rx.w = 0;

    int dragging = 0;
    int last_commit_x = commit.x;
    int last_commit_y = commit.y;

    if (!legacy_mode) {
        const int pid = getpid();
        commit.x = 16 + (pid % 5) * 32;
        commit.y = 16 + (pid % 7) * 24;
        last_commit_x = commit.x;
        last_commit_y = commit.y;
    }

    uint32_t tick = 0;
    while (!g_should_exit) {
        if (legacy_mode) {
            for (;;) {
                uint8_t tmp[128];
                int rn = pipe_try_read(s2c_r_fd, tmp, (uint32_t)sizeof(tmp));
                if (rn < 0) {
                    g_should_exit = 1;
                    break;
                }
                if (rn == 0) break;
                rx_push(&rx, tmp, (uint32_t)rn);
            }

            for (;;) {
                uint32_t avail = rx_count(&rx);
                if (avail < 4) break;

                uint32_t magic;
                rx_peek(&rx, 0, &magic, 4);
                if (magic != COMP_IPC_MAGIC) {
                    rx_drop(&rx, 1);
                    continue;
                }
                if (avail < (uint32_t)sizeof(comp_ipc_hdr_t)) break;

                comp_ipc_hdr_t hdr;
                rx_peek(&rx, 0, &hdr, (uint32_t)sizeof(hdr));
                if (hdr.version != COMP_IPC_VERSION || hdr.len > COMP_IPC_MAX_PAYLOAD) {
                    rx_drop(&rx, 1);
                    continue;
                }

                uint32_t frame_len = (uint32_t)sizeof(hdr) + hdr.len;
                if (avail < frame_len) break;

                rx_drop(&rx, (uint32_t)sizeof(hdr));
                uint8_t payload[COMP_IPC_MAX_PAYLOAD];
                if (hdr.len) {
                    rx_peek(&rx, 0, payload, hdr.len);
                    rx_drop(&rx, hdr.len);
                }

                if (hdr.type == COMP_IPC_MSG_INPUT && hdr.len == (uint32_t)sizeof(comp_ipc_input_t)) {
                    comp_ipc_input_t in;
                    memcpy(&in, payload, sizeof(in));
                    if (in.kind == COMP_IPC_INPUT_MOUSE) {
                        if (in.buttons & 1u) dragging = 1;
                        else dragging = 0;

                        if (dragging) {
                            const int gx = last_commit_x + (int)in.x;
                            const int gy = last_commit_y + (int)in.y;
                            int nx = gx - (width / 2);
                            int ny = gy - (height / 2);
                            if (nx < 0) nx = 0;
                            if (ny < 0) ny = 0;
                            if (nx != last_commit_x || ny != last_commit_y) {
                                last_commit_x = nx;
                                last_commit_y = ny;
                                commit.x = nx;
                                commit.y = ny;
                                if (comp_ipc_send(c2s_w_fd, (uint16_t)COMP_IPC_MSG_COMMIT, seq++, &commit, (uint32_t)sizeof(commit)) < 0) {
                                    g_should_exit = 1;
                                    break;
                                }
                            }
                        }
                    } else if (in.kind == COMP_IPC_INPUT_KEY) {
                        if (in.key_state == 1u) {
                            int step = 8;
                            int nx = last_commit_x;
                            int ny = last_commit_y;

                            uint32_t kc = in.keycode;
                            if (kc == 'a' || kc == 'A' || kc == 0x11u) nx -= step;
                            if (kc == 'd' || kc == 'D' || kc == 0x12u) nx += step;
                            if (kc == 'w' || kc == 'W' || kc == 0x13u) ny -= step;
                            if (kc == 's' || kc == 'S' || kc == 0x14u) ny += step;
                            if (kc == 'r' || kc == 'R') { nx = 64; ny = 64; }

                            if (nx < 0) nx = 0;
                            if (ny < 0) ny = 0;
                            if (nx != last_commit_x || ny != last_commit_y) {
                                last_commit_x = nx;
                                last_commit_y = ny;
                                commit.x = nx;
                                commit.y = ny;
                                if (comp_ipc_send(c2s_w_fd, (uint16_t)COMP_IPC_MSG_COMMIT, seq++, &commit, (uint32_t)sizeof(commit)) < 0) {
                                    g_should_exit = 1;
                                    break;
                                }
                            }
                        }
                    }
                }
            }
        } else {
            for (;;) {
                comp_ipc_hdr_t hdr;
                uint8_t payload[COMP_IPC_MAX_PAYLOAD];
                int r = comp_try_recv(&conn, &hdr, payload, (uint32_t)sizeof(payload));
                if (r < 0) {
                    g_should_exit = 1;
                    break;
                }
                if (r == 0) break;

                if (hdr.type == (uint16_t)COMP_IPC_MSG_INPUT && hdr.len == (uint32_t)sizeof(comp_ipc_input_t)) {
                    comp_ipc_input_t in;
                    memcpy(&in, payload, sizeof(in));
                    if (in.kind == COMP_IPC_INPUT_MOUSE) {
                        if (in.buttons & 1u) dragging = 1;
                        else dragging = 0;

                        if (dragging) {
                            const int gx = last_commit_x + (int)in.x;
                            const int gy = last_commit_y + (int)in.y;
                            int nx = gx - (width / 2);
                            int ny = gy - (height / 2);
                            if (nx < 0) nx = 0;
                            if (ny < 0) ny = 0;
                            if (nx != last_commit_x || ny != last_commit_y) {
                                last_commit_x = nx;
                                last_commit_y = ny;
                                commit.x = nx;
                                commit.y = ny;
                                if (comp_send_commit(&conn, 1u, nx, ny, 0u) != 0) {
                                    g_should_exit = 1;
                                    break;
                                }
                            }
                        }
                    } else if (in.kind == COMP_IPC_INPUT_KEY) {
                        if (in.key_state == 1u) {
                            int step = 8;
                            int nx = last_commit_x;
                            int ny = last_commit_y;

                            uint32_t kc = in.keycode;
                            if (kc == 'a' || kc == 'A' || kc == 0x11u) nx -= step;
                            if (kc == 'd' || kc == 'D' || kc == 0x12u) nx += step;
                            if (kc == 'w' || kc == 'W' || kc == 0x13u) ny -= step;
                            if (kc == 's' || kc == 'S' || kc == 0x14u) ny += step;
                            if (kc == 'r' || kc == 'R') { nx = 64; ny = 64; }

                            if (nx < 0) nx = 0;
                            if (ny < 0) ny = 0;
                            if (nx != last_commit_x || ny != last_commit_y) {
                                last_commit_x = nx;
                                last_commit_y = ny;
                                commit.x = nx;
                                commit.y = ny;
                                if (comp_send_commit(&conn, 1u, nx, ny, 0u) != 0) {
                                    g_should_exit = 1;
                                    break;
                                }
                            }
                        }
                    }
                }
            }
        }

        uint32_t base = 0x101010u;
        uint32_t a = (tick & 255u);
        uint32_t b = ((tick >> 1) & 255u);

        for (int y = 0; y < height; y++) {
            uint32_t* row = pixels + (uint32_t)y * stride;
            for (int x = 0; x < width; x++) {
                uint32_t r = (uint32_t)((x + (int)a) & 255);
                uint32_t g = (uint32_t)((y + (int)b) & 255);
                uint32_t bl = (uint32_t)((x + y + (int)a) & 255);
                row[x] = base ^ ((r << 16) | (g << 8) | bl);
            }
        }

        tick++;
        if (!legacy_mode) {
            comp_wait_events(&conn, 16000u);
        } else {
            usleep(16000);
        }
    }

    munmap((void*)pixels, size_bytes);

    if (!legacy_mode) {
        (void)comp_send_destroy_surface(&conn, 1u, 0u);
        comp_disconnect(&conn);
    } else {
        if (c2s_w_fd >= 0) close(c2s_w_fd);
        if (s2c_r_fd >= 0) close(s2c_r_fd);
    }

    if (shm_fd >= 0) close(shm_fd);
    if (have_shm_name) {
        shm_unlink_named(shm_name);
    }
    return 0;
}
