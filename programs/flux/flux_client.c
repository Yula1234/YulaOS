#include "flux_internal.h"

void comp_buffer_destroy(comp_buffer_t* b) {
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

void comp_client_disconnect(comp_client_t* c) {
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

    if (c->input_ring) {
        munmap((void*)c->input_ring, c->input_ring_size_bytes);
        c->input_ring = 0;
    }
    if (c->input_ring_shm_fd >= 0) {
        close(c->input_ring_shm_fd);
        c->input_ring_shm_fd = -1;
    }
    if (c->input_ring_shm_name[0]) {
        shm_unlink_named(c->input_ring_shm_name);
        c->input_ring_shm_name[0] = '\0';
    }
    c->input_ring_size_bytes = 0;
    c->input_ring_enabled = 0;

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
        for (int bi = 0; bi < COMP_SURFACE_SHADOW_BUFS; bi++) {
            if (s->shadow_pixels[bi] && s->shadow_size_bytes) {
                munmap((void*)s->shadow_pixels[bi], s->shadow_size_bytes);
            }
            s->shadow_pixels[bi] = 0;
            if (s->shadow_shm_fd[bi] >= 0) {
                close(s->shadow_shm_fd[bi]);
            }
            s->shadow_shm_fd[bi] = -1;
        }
        s->shadow_size_bytes = 0;
        s->shadow_stride = 0;
        s->shadow_active = 0;
        s->shadow_valid = 0;
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
        for (int bi = 0; bi < COMP_SURFACE_SHADOW_BUFS; bi++) {
            s->shadow_shm_fd[bi] = -1;
        }
    }
}

comp_surface_t* comp_client_surface_get(comp_client_t* c, uint32_t id, int create) {
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
            s->damage_pending_count = 0u;
            s->damage_committed_count = 0u;
            s->damage_committed_gen = 0u;
            for (int bi = 0; bi < COMP_SURFACE_SHADOW_BUFS; bi++) {
                s->shadow_shm_fd[bi] = -1;
            }
            return s;
        }
    }
    return 0;
}

void comp_client_init(comp_client_t* c, int pid, int fd_c2s, int fd_s2c) {
    if (!c) return;
    memset(c, 0, sizeof(*c));
    c->connected = 1;
    c->pid = pid;
    c->fd_c2s = fd_c2s;
    c->fd_s2c = fd_s2c;
    ipc_rx_reset(&c->rx);

    c->input_ring_shm_fd = -1;
    c->input_ring_size_bytes = 0;
    c->input_ring_shm_name[0] = '\0';
    c->input_ring = 0;
    c->input_ring_enabled = 0;

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
        c->surfaces[i].shm_fd = -1;
        for (int bi = 0; bi < COMP_SURFACE_SHADOW_BUFS; bi++) {
            c->surfaces[i].shadow_shm_fd[bi] = -1;
        }
    }
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

comp_surface_t* comp_client_surface_find(const comp_client_t* c, uint32_t id) {
    if (!c || id == 0) return 0;
    for (int i = 0; i < COMP_MAX_SURFACES; i++) {
        const comp_surface_t* s = &c->surfaces[i];
        if (s->in_use && s->id == id) return (comp_surface_t*)s;
    }
    return 0;
}

int comp_client_surface_id_valid(const comp_client_t* c, uint32_t id) {
    comp_surface_t* s = comp_client_surface_find(c, id);
    return s ? comp_surface_can_receive(s) : 0;
}

int comp_pick_surface_at(comp_client_t* clients, int nclients, int x, int y, int* out_client, uint32_t* out_sid, comp_surface_t** out_s) {
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
