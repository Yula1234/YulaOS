#include "compositor_internal.h"

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

void comp_client_pump(comp_client_t* c, const comp_buffer_t* buf, uint32_t* z_counter, wm_conn_t* wm, uint32_t client_id) {
    if (!c || !c->connected || c->fd_c2s < 0) return;
    if (!z_counter) return;

    int saw_eof = 0;

    for (;;) {
        uint8_t tmp[1024];
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
