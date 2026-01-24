#include "compositor_internal.h"

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
}

void wm_init(wm_conn_t* w, int fd_c2s, int fd_s2c) {
    if (!w) return;
    memset(w, 0, sizeof(*w));
    w->connected = 1;
    w->fd_c2s = fd_c2s;
    w->fd_s2c = fd_s2c;
    ipc_rx_reset(&w->rx);
    w->seq_out = 1;
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

    int wr = pipe_try_write_frame(w->fd_s2c, frame, (uint32_t)sizeof(frame), essential);
    if (wr < 0) return -1;
    if (essential && wr == 0) return -1;
    return 0;
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

void wm_pump(wm_conn_t* w, comp_client_t* clients, int nclients, comp_input_state_t* input, uint32_t* z_counter, comp_preview_t* preview, int* preview_dirty) {
    if (!w || !w->connected || w->fd_c2s < 0) return;
    if (!clients || !input || !z_counter) return;

    int saw_eof = 0;

    for (;;) {
        uint8_t tmp[1024];
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
