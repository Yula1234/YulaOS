#include "flux_internal.h"

void wm_disconnect(wm_conn_t* w) {
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
    w->tx_r = 0;
    w->tx_w = 0;
}

void wm_init(wm_conn_t* w, int fd_c2s, int fd_s2c) {
    if (!w) return;
    memset(w, 0, sizeof(*w));
    w->connected = 1;
    w->fd_c2s = fd_c2s;
    w->fd_s2c = fd_s2c;
    ipc_rx_reset(&w->rx);
    w->seq_out = 1;
    w->tx_r = 0;
    w->tx_w = 0;
}

static inline uint32_t wm_tx_count(const wm_conn_t* w) {
    return w ? (w->tx_w - w->tx_r) : 0u;
}

void wm_flush_tx(wm_conn_t* w) {
    if (!w || !w->connected || w->fd_s2c < 0) return;

    while (w->tx_r != w->tx_w) {
        const uint32_t ri = w->tx_r % 128u;
        uint32_t len = w->tx[ri].len;
        uint32_t off = w->tx[ri].off;
        if (off >= len) {
            w->tx_r++;
            continue;
        }

        int wn = pipe_try_write(w->fd_s2c, w->tx[ri].frame + off, len - off);
        if (wn < 0) {
            wm_disconnect(w);
            return;
        }
        if (wn == 0) {
            return;
        }

        w->tx[ri].off = off + (uint32_t)wn;
        if (w->tx[ri].off >= w->tx[ri].len) {
            w->tx_r++;
        }
    }
}

int wm_send_event(wm_conn_t* w, const comp_ipc_wm_event_t* ev, int essential) {
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

    if (!essential) {
        wm_flush_tx(w);
        if (!w->connected) return -1;
        if (wm_tx_count(w) != 0u) {
            return 0;
        }
        int wr = pipe_try_write_frame(w->fd_s2c, frame, (uint32_t)sizeof(frame), 0);
        if (wr < 0) return -1;
        return 0;
    }

    if (wm_tx_count(w) >= 128u) {
        wm_flush_tx(w);
        if (wm_tx_count(w) >= 128u) {
            return -1;
        }
    }

    const uint32_t wi = w->tx_w % 128u;
    w->tx[wi].len = (uint32_t)sizeof(frame);
    w->tx[wi].off = 0;
    memcpy(w->tx[wi].frame, frame, (uint32_t)sizeof(frame));
    w->tx_w++;

    wm_flush_tx(w);
    return w->connected ? 0 : -1;
}

void wm_replay_state(wm_conn_t* wm, const comp_client_t* clients, int nclients) {
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

void wm_pump(wm_conn_t* w, comp_client_t* clients, int nclients, comp_input_state_t* input, uint32_t* z_counter, comp_preview_t* preview, int* preview_dirty, int* scene_dirty) {
    if (!w || !w->connected || w->fd_c2s < 0) return;
    if (!clients || !input || !z_counter) return;

    wm_flush_tx(w);

    int saw_eof = 0;

    for (;;) {
        const uint32_t cap = (uint32_t)sizeof(w->rx.buf);
        uint32_t count = ipc_rx_count(&w->rx);
        uint32_t space = (count < cap) ? (cap - count) : 0u;
        const uint32_t reserve = (uint32_t)sizeof(comp_ipc_hdr_t) + (uint32_t)COMP_IPC_MAX_PAYLOAD;
        if (space <= reserve) break;
        space -= reserve;

        uint8_t tmp[1024];
        uint32_t want = space;
        if (want > (uint32_t)sizeof(tmp)) want = (uint32_t)sizeof(tmp);
        int rn = pipe_try_read(w->fd_c2s, tmp, want);
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

            if (cmd.kind == COMP_WM_CMD_KEYBOARD_GRAB) {
                if (cmd.flags & 1u) {
                    input->wm_keyboard_grab_active = 1;
                } else {
                    input->wm_keyboard_grab_active = 0;
                }
                continue;
            }

            if (cmd.kind == COMP_WM_CMD_EXIT) {
                char tmp[80];
                (void)snprintf(tmp, sizeof(tmp), "flux: wm exit cmd from %u\n", (unsigned)cmd.client_id);
                dbg_write(tmp);
                g_should_exit = 1;
                wm_disconnect(w);
                return;
            }

            if (cmd.kind == COMP_WM_CMD_CLOSE) {
                if (cmd.client_id >= (uint32_t)nclients || cmd.surface_id == 0) continue;
                comp_client_t* c = &clients[(int)cmd.client_id];
                if (!c->connected) continue;

                int pid = c->pid;
                if (pid > 0) {
                    if (input->focus_client == (int)cmd.client_id) {
                        input->focus_client = -1;
                        input->focus_surface_id = 0;
                    }

                    comp_ipc_input_t in;
                    memset(&in, 0, sizeof(in));
                    in.surface_id = cmd.surface_id;
                    in.kind = COMP_IPC_INPUT_CLOSE;

                    int sent = 0;
                    if (c->input_ring_enabled && c->input_ring && (c->input_ring->flags & COMP_INPUT_RING_FLAG_READY)) {
                        if (comp_client_send_input(c, &in, 1) < 0) {
                            sent = 0;
                        } else {
                            sent = 1;
                        }
                    } else {
                        if (c->connected && c->fd_s2c >= 0) {
                            comp_ipc_hdr_t hdr;
                            hdr.magic = COMP_IPC_MAGIC;
                            hdr.version = (uint16_t)COMP_IPC_VERSION;
                            hdr.type = (uint16_t)COMP_IPC_MSG_INPUT;
                            hdr.len = (uint32_t)sizeof(in);
                            hdr.seq = c->seq_out++;

                            uint8_t frame[sizeof(hdr) + sizeof(in)];
                            memcpy(frame, &hdr, (uint32_t)sizeof(hdr));
                            memcpy(frame + sizeof(hdr), &in, (uint32_t)sizeof(in));

                            int wr = pipe_try_write_frame(c->fd_s2c, frame, (uint32_t)sizeof(frame), 1);
                            sent = (wr == 1);
                        }
                    }

                    if (!sent) {
                        (void)syscall(9, pid, 0, 0);
                    }
                }

                continue;
            }

            if (cmd.kind == COMP_WM_CMD_FOCUS) {
                if (cmd.client_id >= (uint32_t)nclients || cmd.surface_id == 0) {
                    if (input->focus_client != -1 || input->focus_surface_id != 0) {
                        input->focus_client = -1;
                        input->focus_surface_id = 0;
                        if (scene_dirty) *scene_dirty = 1;
                    }
                } else {
                    comp_client_t* c = &clients[(int)cmd.client_id];
                    if (!c->connected) continue;
                    comp_surface_t* s = comp_client_surface_get(c, cmd.surface_id, 0);
                    if (!s || !s->attached || !s->committed) continue;

                    if (input->focus_client != (int)cmd.client_id || input->focus_surface_id != cmd.surface_id) {
                        input->focus_client = (int)cmd.client_id;
                        input->focus_surface_id = cmd.surface_id;
                        if (scene_dirty) *scene_dirty = 1;
                    }
                }
                continue;
            }

            if (cmd.client_id >= (uint32_t)nclients || cmd.surface_id == 0) continue;

            comp_client_t* c = &clients[(int)cmd.client_id];
            if (!c->connected) continue;

            comp_surface_t* s = comp_client_surface_get(c, cmd.surface_id, 0);
            if (!s || !s->attached || !s->committed) continue;

            if (cmd.kind == COMP_WM_CMD_RAISE) {
                s->z = ++(*z_counter);
                if (scene_dirty) *scene_dirty = 1;
            } else if (cmd.kind == COMP_WM_CMD_MOVE) {
                s->x = (int)cmd.x;
                s->y = (int)cmd.y;
                if (scene_dirty) *scene_dirty = 1;
            } else if (cmd.kind == COMP_WM_CMD_RESIZE) {
                if (cmd.x <= 0 || cmd.y <= 0) continue;
                if (c->fd_s2c < 0) continue;

                if (scene_dirty) *scene_dirty = 1;

                comp_ipc_input_t in;
                in.surface_id = cmd.surface_id;
                in.kind = COMP_IPC_INPUT_RESIZE;
                in.x = cmd.x;
                in.y = (int32_t)cmd.y;
                in.buttons = 0;
                in.keycode = 0;
                in.key_state = 0;

                (void)comp_client_send_input(c, &in, 1);
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
                    if (scene_dirty) *scene_dirty = 1;
                }
            } else if (cmd.kind == COMP_WM_CMD_PREVIEW_CLEAR) {
                if (!preview) continue;
                if (preview->active && preview->client_id == cmd.client_id && preview->surface_id == cmd.surface_id) {
                    preview->active = 0;
                    if (preview_dirty) *preview_dirty = 1;
                    if (scene_dirty) *scene_dirty = 1;
                }
            }
        }
    }

    if (saw_eof) {
        input->wm_pointer_grab_active = 0;
        input->wm_pointer_grab_client = -1;
        input->wm_pointer_grab_surface_id = 0;
        input->wm_keyboard_grab_active = 0;
        wm_disconnect(w);
    }
}
