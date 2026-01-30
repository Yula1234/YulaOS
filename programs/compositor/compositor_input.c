#include "compositor_internal.h"

int comp_client_send_input(comp_client_t* c, const comp_ipc_input_t* in, int essential) {
    if (!c || !in) return 0;
    if (!c->connected || c->fd_s2c < 0) return 0;

    if (c->input_ring_enabled && c->input_ring && (c->input_ring->flags & COMP_INPUT_RING_FLAG_READY)) {
        comp_input_ring_t* ring = c->input_ring;

        if (c->input_ring_mouse_pending) {
            for (;;) {
                const uint32_t r = ring->r;
                const uint32_t w = ring->w;
                const uint32_t used = w - r;
                if (used >= ring->cap) break;

                const uint32_t wi = w & ring->mask;
                ring->events[wi] = c->input_ring_mouse_pending_ev;
                __sync_synchronize();
                ring->w = w + 1u;
                __sync_synchronize();

                if (ring->flags & COMP_INPUT_RING_FLAG_WAIT_R) {
                    (void)__sync_fetch_and_and(&ring->flags, ~COMP_INPUT_RING_FLAG_WAIT_R);
                    futex_wake(&ring->w, 1u);
                }

                c->input_ring_mouse_pending = 0;
                break;
            }
        }

        for (;;) {
            const uint32_t r = ring->r;
            const uint32_t w = ring->w;
            const uint32_t used = w - r;
            if (used >= ring->cap) {
                if (in->kind == COMP_IPC_INPUT_MOUSE) {
                    ring->dropped++;
                    c->input_ring_mouse_pending_ev = *in;
                    c->input_ring_mouse_pending = 1;
                    if (ring->flags & COMP_INPUT_RING_FLAG_WAIT_R) {
                        (void)__sync_fetch_and_and(&ring->flags, ~COMP_INPUT_RING_FLAG_WAIT_R);
                        futex_wake(&ring->w, 1u);
                    }
                    return 0;
                }

                if (essential) {
                    (void)__sync_fetch_and_or(&ring->flags, COMP_INPUT_RING_FLAG_WAIT_W);
                    __sync_synchronize();

                    const uint32_t r2 = ring->r;
                    const uint32_t w2 = ring->w;
                    if ((w2 - r2) < ring->cap) {
                        (void)__sync_fetch_and_and(&ring->flags, ~COMP_INPUT_RING_FLAG_WAIT_W);
                        continue;
                    }

                    (void)futex_wait(&ring->r, r);
                    (void)__sync_fetch_and_and(&ring->flags, ~COMP_INPUT_RING_FLAG_WAIT_W);
                    continue;
                }

                ring->dropped++;
                return 0;
            }

            const uint32_t wi = w & ring->mask;
            ring->events[wi] = *in;
            __sync_synchronize();
            ring->w = w + 1u;
            __sync_synchronize();

            if (ring->flags & COMP_INPUT_RING_FLAG_WAIT_R) {
                (void)__sync_fetch_and_and(&ring->flags, ~COMP_INPUT_RING_FLAG_WAIT_R);
                futex_wake(&ring->w, 1u);
            }
            return 0;
        }
    }

    comp_ipc_hdr_t hdr;
    hdr.magic = COMP_IPC_MAGIC;
    hdr.version = (uint16_t)COMP_IPC_VERSION;
    hdr.type = (uint16_t)COMP_IPC_MSG_INPUT;
    hdr.len = (uint32_t)sizeof(*in);
    hdr.seq = c->seq_out++;

    uint8_t frame[sizeof(hdr) + sizeof(*in)];
    memcpy(frame, &hdr, sizeof(hdr));
    memcpy(frame + sizeof(hdr), in, sizeof(*in));

    int wr = pipe_try_write_frame(c->fd_s2c, frame, (uint32_t)sizeof(frame), essential);
    if (wr < 0) return -1;
    return 0;
}

void comp_input_state_init(comp_input_state_t* st) {
    if (!st) return;
    memset(st, 0, sizeof(*st));
    st->focus_client = -1;
    st->grab_client = -1;
    st->grab_active = 0;
    st->wm_pointer_grab_active = 0;
    st->wm_pointer_grab_client = -1;
    st->wm_pointer_grab_surface_id = 0;
    st->wm_keyboard_grab_active = 0;
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

void comp_send_wm_pointer(wm_conn_t* wm, comp_client_t* clients, int nclients, comp_input_state_t* st, const mouse_state_t* ms) {
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

void comp_update_focus(comp_client_t* clients, int nclients, comp_input_state_t* st, const mouse_state_t* ms, uint32_t* z_counter, wm_conn_t* wm) {
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

    if (st->focus_client >= 0 && st->focus_client < nclients && st->focus_surface_id != 0) {
        comp_client_t* fc = &clients[st->focus_client];
        if (!fc->connected) {
            st->focus_client = -1;
            st->focus_surface_id = 0;
        } else if (wm && wm->connected) {
            if (!comp_client_surface_find(fc, st->focus_surface_id)) {
                st->focus_client = -1;
                st->focus_surface_id = 0;
            }
        } else {
            if (!comp_client_surface_id_valid(fc, st->focus_surface_id)) {
                st->focus_client = -1;
                st->focus_surface_id = 0;
            }
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

int comp_send_mouse(comp_client_t* clients, int nclients, comp_input_state_t* st, const mouse_state_t* ms) {
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

    if (comp_client_send_input(c, &in, 0) < 0) {
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

int comp_send_key(comp_client_t* clients, int nclients, comp_input_state_t* st, uint32_t keycode, uint32_t key_state) {
    if (!st) return 0;
    if (st->wm_keyboard_grab_active) return 0;
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

    if (comp_client_send_input(c, &in, 1) < 0) return -1;
    return 0;
}
