// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#include <yula.h>
#include <comp.h>

static volatile int g_should_exit;

static void on_signal(int sig) {
    (void)sig;
    g_should_exit = 1;
    sigreturn();
    for (;;) {
    }
}

static inline uint32_t rgb(uint32_t r, uint32_t g, uint32_t b) {
    return ((r & 255u) << 16) | ((g & 255u) << 8) | (b & 255u);
}

typedef struct {
    uint32_t id;
    int alive;

    int x;
    int y;
    int w;
    int h;

    uint32_t* pixels;
    uint32_t size_bytes;
    uint32_t stride;

    int shm_fd;
    char shm_name[32];

    int dragging;
    int drag_off_x;
    int drag_off_y;

    uint32_t base_color;
} surf_t;

static void surf_draw(surf_t* s, uint32_t tick) {
    if (!s || !s->alive || !s->pixels) return;

    const int w = s->w;
    const int h = s->h;
    const uint32_t bc = s->base_color;

    uint32_t t = tick;
    uint32_t rr = ((bc >> 16) & 255u);
    uint32_t gg = ((bc >> 8) & 255u);
    uint32_t bb = (bc & 255u);

    rr = (rr + (t & 31u)) & 255u;
    gg = (gg + ((t >> 1) & 31u)) & 255u;
    bb = (bb + ((t >> 2) & 31u)) & 255u;

    const uint32_t fill = rgb(rr, gg, bb);
    const uint32_t border1 = 0xFFFFFFu;
    const uint32_t border2 = 0x000000u;

    for (int y = 0; y < h; y++) {
        uint32_t* row = s->pixels + (uint32_t)y * s->stride;
        for (int x = 0; x < w; x++) {
            uint32_t c = fill;
            if (x == 0 || y == 0 || x == (w - 1) || y == (h - 1)) c = border1;
            if (x == 1 || y == 1 || x == (w - 2) || y == (h - 2)) c = border2;
            row[x] = c;
        }
    }

    int cx = w / 2;
    int cy = h / 2;
    for (int i = -8; i <= 8; i++) {
        int xx = cx + i;
        int yy = cy + i;
        if ((unsigned)xx < (unsigned)w && (unsigned)cy < (unsigned)h) s->pixels[(uint32_t)cy * s->stride + (uint32_t)xx] = 0xFFFF00u;
        if ((unsigned)cx < (unsigned)w && (unsigned)yy < (unsigned)h) s->pixels[(uint32_t)yy * s->stride + (uint32_t)cx] = 0xFFFF00u;
    }
}

static int surf_create(comp_conn_t* c, surf_t* s, uint32_t id, int w, int h, int x, int y, uint32_t base_color) {
    if (!c || !c->connected || !s) return -1;
    if (id == 0 || w <= 0 || h <= 0) return -1;

    memset(s, 0, sizeof(*s));
    s->id = id;
    s->w = w;
    s->h = h;
    s->x = x;
    s->y = y;
    s->stride = (uint32_t)w;
    s->size_bytes = (uint32_t)w * (uint32_t)h * 4u;
    s->shm_fd = -1;
    s->pixels = 0;
    s->base_color = base_color;

    int pid = getpid();
    int created = 0;
    for (int i = 0; i < 8; i++) {
        (void)snprintf(s->shm_name, sizeof(s->shm_name), "wt_%d_%u_%d", pid, (unsigned)id, i);
        int fd = shm_create_named(s->shm_name, s->size_bytes);
        if (fd >= 0) {
            s->shm_fd = fd;
            created = 1;
            break;
        }
    }
    if (!created) return -1;

    s->pixels = (uint32_t*)mmap(s->shm_fd, s->size_bytes, MAP_SHARED);
    if (!s->pixels) {
        close(s->shm_fd);
        s->shm_fd = -1;
        shm_unlink_named(s->shm_name);
        return -1;
    }

    surf_draw(s, 0);

    uint16_t err = 0;
    if (comp_send_attach_shm_name_sync(c, s->id, s->shm_name, s->size_bytes, (uint32_t)s->w, (uint32_t)s->h, s->stride, 0u, 2000u, &err) != 0) {
        munmap((void*)s->pixels, s->size_bytes);
        s->pixels = 0;
        close(s->shm_fd);
        s->shm_fd = -1;
        shm_unlink_named(s->shm_name);
        return -1;
    }

    if (comp_send_commit_sync(c, s->id, s->x, s->y, 0u, 2000u, &err) != 0) {
        (void)comp_send_destroy_surface_sync(c, s->id, 0u, 2000u, 0);
        munmap((void*)s->pixels, s->size_bytes);
        s->pixels = 0;
        close(s->shm_fd);
        s->shm_fd = -1;
        shm_unlink_named(s->shm_name);
        return -1;
    }

    s->alive = 1;
    return 0;
}

static void surf_destroy(comp_conn_t* c, surf_t* s) {
    if (!s || !s->alive) return;

    if (c && c->connected) {
        (void)comp_send_destroy_surface_sync(c, s->id, 0u, 2000u, 0);
    }

    if (s->pixels) {
        munmap((void*)s->pixels, s->size_bytes);
        s->pixels = 0;
    }
    if (s->shm_fd >= 0) {
        close(s->shm_fd);
        s->shm_fd = -1;
    }
    if (s->shm_name[0]) {
        shm_unlink_named(s->shm_name);
        s->shm_name[0] = '\0';
    }

    s->alive = 0;
    s->dragging = 0;
}

static surf_t* find_surface(surf_t* surfs, int nsurfs, uint32_t id) {
    if (!surfs) return 0;
    for (int i = 0; i < nsurfs; i++) {
        if (surfs[i].alive && surfs[i].id == id) return &surfs[i];
    }
    return 0;
}

static void handle_mouse(comp_conn_t* c, surf_t* s, const comp_ipc_input_t* in) {
    if (!c || !s || !in) return;

    const int down = ((in->buttons & 1u) != 0);
    if (down && !s->dragging) {
        s->dragging = 1;
        s->drag_off_x = (int)in->x;
        s->drag_off_y = (int)in->y;
    }
    if (!down && s->dragging) {
        s->dragging = 0;
    }

    if (s->dragging) {
        int nx = s->x + (int)in->x - s->drag_off_x;
        int ny = s->y + (int)in->y - s->drag_off_y;
        if (nx != s->x || ny != s->y) {
            s->x = nx;
            s->y = ny;
            (void)comp_send_commit(c, s->id, s->x, s->y, 0u);
        }
    }
}

static void handle_key(comp_conn_t* c, surf_t* surfs, int nsurfs, const comp_ipc_input_t* in) {
    if (!c || !surfs || !in) return;
    if (in->key_state == 0) return;

    surf_t* s = find_surface(surfs, nsurfs, in->surface_id);
    char k = (char)(uint8_t)in->keycode;

    if (k == 'q') {
        g_should_exit = 1;
        return;
    }

    if (k == 'z') {
        if (s) {
            (void)comp_send_commit(c, s->id, s->x, s->y, COMP_IPC_COMMIT_FLAG_RAISE);
        }
        return;
    }

    if (k == 'x') {
        if (s) {
            surf_destroy(c, s);
        }
        return;
    }

    if (k == '1' && nsurfs >= 1) {
        if (surfs[0].alive) {
            if (in->surface_id == surfs[0].id) {
                surf_destroy(c, &surfs[0]);
            }
        } else {
            (void)surf_create(c, &surfs[0], 1u, 240, 180, 60, 60, rgb(200, 70, 70));
        }
        return;
    }

    if (k == '2' && nsurfs >= 2) {
        if (surfs[1].alive) {
            if (in->surface_id == surfs[1].id) {
                surf_destroy(c, &surfs[1]);
            }
        } else {
            (void)surf_create(c, &surfs[1], 2u, 240, 180, 120, 110, rgb(70, 90, 210));
        }
        return;
    }

    if (k == 'r') {
        for (int i = 0; i < nsurfs; i++) {
            if (surfs[i].alive) surf_destroy(c, &surfs[i]);
        }
        if (nsurfs >= 1) (void)surf_create(c, &surfs[0], 1u, 240, 180, 60, 60, rgb(200, 70, 70));
        if (nsurfs >= 2) (void)surf_create(c, &surfs[1], 2u, 240, 180, 120, 110, rgb(70, 90, 210));
        return;
    }
}

__attribute__((force_align_arg_pointer)) int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    signal(2, (void*)on_signal);
    signal(15, (void*)on_signal);

    comp_conn_t c;
    comp_conn_reset(&c);

    while (!g_should_exit) {
        if (!c.connected) {
            if (comp_connect(&c, "compositor") != 0) {
                usleep(100000);
                continue;
            }

            uint16_t err = 0;
            if (comp_send_hello_sync(&c, 2000u, &err) != 0) {
                comp_disconnect(&c);
                usleep(100000);
                continue;
            }
        }
        break;
    }

    if (!c.connected) {
        return 1;
    }

    surf_t surfs[2];
    memset(surfs, 0, sizeof(surfs));

    (void)surf_create(&c, &surfs[0], 1u, 240, 180, 60, 60, rgb(200, 70, 70));
    (void)surf_create(&c, &surfs[1], 2u, 240, 180, 120, 110, rgb(70, 90, 210));

    uint32_t tick = 0;

    while (!g_should_exit) {
        for (;;) {
            comp_ipc_hdr_t hdr;
            uint8_t payload[COMP_IPC_MAX_PAYLOAD];
            int r = comp_try_recv(&c, &hdr, payload, (uint32_t)sizeof(payload));
            if (r < 0) {
                g_should_exit = 1;
                break;
            }
            if (r == 0) break;

            if (hdr.type == (uint16_t)COMP_IPC_MSG_INPUT && hdr.len == (uint32_t)sizeof(comp_ipc_input_t)) {
                comp_ipc_input_t in;
                memcpy(&in, payload, sizeof(in));
                surf_t* s = find_surface(surfs, 2, in.surface_id);
                if (in.kind == COMP_IPC_INPUT_MOUSE) {
                    if (s) handle_mouse(&c, s, &in);
                } else if (in.kind == COMP_IPC_INPUT_KEY) {
                    handle_key(&c, surfs, 2, &in);
                }
            }
        }

        tick++;
        for (int i = 0; i < 2; i++) {
            surf_draw(&surfs[i], tick);
        }

        usleep(16000);
    }

    for (int i = 0; i < 2; i++) {
        surf_destroy(&c, &surfs[i]);
    }

    comp_disconnect(&c);
    return 0;
}
