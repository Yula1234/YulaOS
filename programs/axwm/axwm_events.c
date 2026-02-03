#include "axwm_internal.h"

int wm_pick_next_focus_idx(const wm_state_t* st, int start_idx) {
    if (!st) return -1;
    if (start_idx < 0 || start_idx >= WM_MAX_VIEWS) start_idx = 0;

    for (int step = 1; step <= WM_MAX_VIEWS; step++) {
        int idx = (start_idx + step) % WM_MAX_VIEWS;
        const wm_view_t* v = &st->views[idx];
        if (wm_is_view_visible_on_active_ws(st, v) && !v->ui) {
            return idx;
        }
    }
    return -1;
}

static void wm_on_map(comp_conn_t* c, wm_state_t* st, const comp_ipc_wm_event_t* ev) {
    if (!c || !st || !ev) return;
    if (ev->surface_id == 0) return;
    if (ev->flags & COMP_WM_EVENT_FLAG_BACKGROUND) return;

    if (ev->surface_id == WM_UI_BAR_SURFACE_ID) {
        st->ui.client_id = ev->client_id;
        st->ui.surface_id = WM_UI_BAR_SURFACE_ID;
        if (ev->sw) st->ui.w = ev->sw;
        if (ev->sh) st->ui.h = ev->sh;
        wm_view_t* v = wm_get_or_create_view(st, ev->client_id, ev->surface_id);
        if (!v) return;
        v->ui = 1;
        v->floating = 1;
        v->hidden = 0;
        v->x = 0;
        v->y = 0;
        v->w = ev->sw;
        v->h = ev->sh;
        wm_ui_raise_and_place(c, st);
        wm_apply_layout(c, st);
        return;
    }

    const int existed = (wm_find_view_idx(st, ev->client_id, ev->surface_id) >= 0);
    wm_view_t* v = wm_get_or_create_view(st, ev->client_id, ev->surface_id);
    if (!v) return;
    v->w = ev->sw;
    v->h = ev->sh;
    v->x = ev->sx;
    v->y = ev->sy;
    v->hidden = 0;

    const int view_idx = (int)(v - st->views);
    if (!existed && !v->floating) {
        uint32_t ws = v->workspace;
        if (ws < WM_MAX_WORKSPACES) {
            if (st->layout_root[ws] < 0) {
                int n = wm_layout_alloc_node(st, ws);
                if (n >= 0) {
                    st->layout_nodes[n].is_split = 0;
                    st->layout_nodes[n].view_idx = view_idx;
                    st->layout_root[ws] = n;
                }
            } else {
                int split_on = -1;

                if (st->focused_idx >= 0 && st->focused_idx < WM_MAX_VIEWS) {
                    wm_view_t* fv = &st->views[st->focused_idx];
                    if (fv->mapped && !fv->ui && !fv->floating && fv->workspace == ws) {
                        split_on = st->focused_idx;
                    }
                }

                if (split_on < 0) {
                    int leaf = wm_layout_find_any_leaf(st, ws);
                    if (leaf >= 0) split_on = st->layout_nodes[leaf].view_idx;
                }

                if (split_on >= 0 && split_on < WM_MAX_VIEWS && split_on != view_idx) {
                    wm_layout_insert_split(st, ws, split_on, view_idx);
                }
            }
        }
    }

    if (!existed) {
        char tmp[96];
        (void)snprintf(tmp, sizeof(tmp), "axwm: map c=%u s=%u %ux%u at %d,%d\n", (unsigned)ev->client_id,
                      (unsigned)ev->surface_id, (unsigned)ev->sw, (unsigned)ev->sh, (int)ev->sx, (int)ev->sy);
        dbg_write(tmp);
    }

    if (st->master_surface_id[v->workspace] == 0 && !v->floating) {
        wm_master_set_for_ws(st, v->workspace, v->client_id, v->surface_id);
    }

    if (!(ev->flags & COMP_WM_EVENT_FLAG_REPLAY)) {
        wm_apply_layout(c, st);
        int idx = wm_find_view_idx(st, ev->client_id, ev->surface_id);
        if (idx >= 0) wm_focus_view_idx(c, st, idx);
    } else {
        if (v->workspace != st->active_ws) wm_hide_view(c, v);
        if (st->focused_idx < 0 && v->workspace == st->active_ws) {
            int idx = wm_find_view_idx(st, ev->client_id, ev->surface_id);
            if (idx >= 0) wm_focus_view_idx(c, st, idx);
        }
        wm_apply_layout(c, st);
    }
}

static void wm_on_unmap(comp_conn_t* c, wm_state_t* st, const comp_ipc_wm_event_t* ev) {
    if (!c || !st || !ev) return;
    int idx = wm_find_view_idx(st, ev->client_id, ev->surface_id);
    if (idx < 0) return;

    if (st->views[idx].ui || ev->surface_id == WM_UI_BAR_SURFACE_ID) {
        if (st->ui.client_id == ev->client_id && st->ui.surface_id == ev->surface_id) {
            st->ui.client_id = COMP_WM_CLIENT_NONE;
        }
        wm_drop_view(st, idx);
        return;
    }
    if (st->drag_active && st->drag_view_idx == idx) {
        wm_stop_drag(c, st);
    }
    int was_focused = (st->focused_idx == idx);

    uint32_t ws = st->views[idx].workspace;
    int was_master = wm_master_matches(st, ws, &st->views[idx]);

    if (!st->views[idx].floating) {
        wm_layout_remove_view(st, ws, idx);
    }
    wm_drop_view(st, idx);
    if (was_focused) wm_clear_focus(st);

    if (was_master) {
        wm_master_clear_for_ws(st, ws);
        wm_reselect_master_for_ws(st, ws);
    }

    if (was_focused || st->focused_idx < 0) {
        int next_idx = wm_pick_next_focus_idx(st, idx);
        if (next_idx >= 0) {
            wm_focus_view_idx(c, st, next_idx);
        } else {
            wm_clear_focus(st);
            wm_ui_draw_bar(st);
            wm_ui_raise_and_place(c, st);
        }
    }
}

static void wm_on_commit(comp_conn_t* c, wm_state_t* st, const comp_ipc_wm_event_t* ev) {
    if (!c || !st || !ev) return;
    if (ev->surface_id == 0) return;
    if (ev->flags & COMP_WM_EVENT_FLAG_BACKGROUND) return;

    wm_view_t* v = wm_get_or_create_view(st, ev->client_id, ev->surface_id);
    if (!v) return;

    if (v->floating) {
        v->w = ev->sw;
        v->h = ev->sh;
    }

    if (ev->surface_id == WM_UI_BAR_SURFACE_ID || v->ui) {
        v->ui = 1;
        v->floating = 1;
        v->hidden = 0;
        v->x = 0;
        v->y = 0;
        st->ui.client_id = ev->client_id;
        st->ui.surface_id = WM_UI_BAR_SURFACE_ID;
        if (v->w) st->ui.w = v->w;
        if (v->h) st->ui.h = v->h;
        wm_ui_raise_and_place(c, st);
        return;
    }

    (void)c;
    (void)st;
}

static void wm_on_click(comp_conn_t* c, wm_state_t* st, const comp_ipc_wm_event_t* ev) {
    if (!c || !st || !ev) return;
    if (ev->surface_id == 0) return;
    if (ev->flags & COMP_WM_EVENT_FLAG_BACKGROUND) return;

    if (ev->surface_id == WM_UI_BAR_SURFACE_ID) {
        return;
    }

    int idx = wm_find_view_idx(st, ev->client_id, ev->surface_id);
    if (idx < 0) return;
    wm_focus_view_idx(c, st, idx);
}

static void wm_on_pointer(comp_conn_t* c, wm_state_t* st, const comp_ipc_wm_event_t* ev) {
    if (!c || !st || !ev) return;

    uint32_t prev = st->pointer_buttons;
    uint32_t cur = ev->buttons;
    const uint32_t left_mask = 1u;
    const uint32_t right_mask = 2u;
    const uint32_t middle_mask = 4u;
    const int left_pressed = ((cur & left_mask) != 0u) && ((prev & left_mask) == 0u);
    const int right_pressed = ((cur & right_mask) != 0u) && ((prev & right_mask) == 0u);
    const int middle_pressed = ((cur & middle_mask) != 0u) && ((prev & middle_mask) == 0u);

    st->pointer_buttons = cur;
    st->pointer_x = ev->px;
    st->pointer_y = ev->py;

    if (ev->surface_id == WM_UI_BAR_SURFACE_ID && !(ev->flags & COMP_WM_EVENT_FLAG_BACKGROUND)) {
        if (left_pressed) {
            const int32_t lx = ev->px - ev->sx;
            wm_ui_handle_bar_click(c, st, lx);
            return;
        }
    }

    if (st->drag_active) {
        const uint32_t bm = st->drag_button_mask;
        const int drag_button_released = (bm != 0u) && ((cur & bm) == 0u) && ((prev & bm) != 0u);
        if (drag_button_released || (st->drag_requires_super && !st->super_down)) {
            wm_stop_drag(c, st);
            return;
        }

        int idx = st->drag_view_idx;
        if (idx < 0 || idx >= WM_MAX_VIEWS) {
            wm_stop_drag(c, st);
            return;
        }
        wm_view_t* v = &st->views[idx];
        if (!wm_is_view_visible_on_active_ws(st, v) || !v->floating) {
            wm_stop_drag(c, st);
            return;
        }

        if (st->drag_is_resize) {
            int32_t dx = ev->px - st->drag_start_px;
            int32_t dy = ev->py - st->drag_start_py;

            int32_t nx = st->drag_resize_start_x;
            int32_t ny = st->drag_resize_start_y;
            int32_t nw = (int32_t)st->drag_resize_start_w;
            int32_t nh = (int32_t)st->drag_resize_start_h;

            if (st->drag_resize_edges & WM_RESIZE_EDGE_LEFT) {
                nx += dx;
                nw -= dx;
            }
            if (st->drag_resize_edges & WM_RESIZE_EDGE_RIGHT) {
                nw += dx;
            }
            if (st->drag_resize_edges & WM_RESIZE_EDGE_TOP) {
                ny += dy;
                nh -= dy;
            }
            if (st->drag_resize_edges & WM_RESIZE_EDGE_BOTTOM) {
                nh += dy;
            }

            if (nw < (int32_t)WM_RESIZE_MIN_W) {
                if (st->drag_resize_edges & WM_RESIZE_EDGE_LEFT) {
                    nx = st->drag_resize_start_x + (int32_t)st->drag_resize_start_w -
                         (int32_t)WM_RESIZE_MIN_W;
                }
                nw = (int32_t)WM_RESIZE_MIN_W;
            }
            if (nh < (int32_t)WM_RESIZE_MIN_H) {
                if (st->drag_resize_edges & WM_RESIZE_EDGE_TOP) {
                    ny = st->drag_resize_start_y + (int32_t)st->drag_resize_start_h -
                         (int32_t)WM_RESIZE_MIN_H;
                }
                nh = (int32_t)WM_RESIZE_MIN_H;
            }

            st->drag_resize_new_x = nx;
            st->drag_resize_new_y = ny;
            st->drag_resize_new_w = (uint32_t)nw;
            st->drag_resize_new_h = (uint32_t)nh;

            if (nx != v->x || ny != v->y) {
                v->x = nx;
                v->y = ny;
                (void)comp_wm_move(c, v->client_id, v->surface_id, v->x, v->y);
            }

            if ((uint32_t)nw != st->drag_preview_last_w || (uint32_t)nh != st->drag_preview_last_h) {
                st->drag_preview_last_w = (uint32_t)nw;
                st->drag_preview_last_h = (uint32_t)nh;
                (void)comp_wm_preview_rect(c, v->client_id, v->surface_id, nw, nh);
            }
        } else {
            int32_t nx = ev->px - st->drag_off_x;
            int32_t ny = ev->py - st->drag_off_y;
            if (nx != v->x || ny != v->y) {
                v->x = nx;
                v->y = ny;
                (void)comp_wm_move(c, v->client_id, v->surface_id, v->x, v->y);
            }
        }
        return;
    }

    if (right_pressed && st->super_down) {
        if (ev->flags & COMP_WM_EVENT_FLAG_BACKGROUND) return;
        if (ev->surface_id == 0) return;
        int idx = wm_find_view_idx(st, ev->client_id, ev->surface_id);
        if (idx < 0) return;
        wm_focus_view_idx(c, st, idx);
        wm_view_t* v = &st->views[idx];
        uint32_t edges = wm_resize_edges_for_point(v, ev->px, ev->py);
        if (edges == 0) {
            edges = WM_RESIZE_EDGE_RIGHT | WM_RESIZE_EDGE_BOTTOM;
        }
        wm_start_resize(c, st, idx, ev->px, ev->py, right_mask, edges);
        st->drag_requires_super = 1;
        return;
    }

    if (left_pressed && st->super_down) {
        if (ev->flags & COMP_WM_EVENT_FLAG_BACKGROUND) return;
        if (ev->surface_id == 0) return;
        int idx = wm_find_view_idx(st, ev->client_id, ev->surface_id);
        if (idx < 0) return;
        wm_focus_view_idx(c, st, idx);
        wm_start_drag(c, st, idx, ev->px, ev->py, left_mask, 1);
        return;
    }
}

static void wm_on_key(comp_conn_t* c, wm_state_t* st, const comp_ipc_wm_event_t* ev) {
    if (!c || !st || !ev) return;
    if (ev->key_state == 0) return;

    uint8_t kc = (uint8_t)ev->keycode;

    if (kc == 0xC0u) {
        st->super_down = 1;
        return;
    }
    if (kc == 0xC1u) {
        st->super_down = 0;
        if (st->drag_active) wm_stop_drag(c, st);
        return;
    }

    if (kc >= 0x90u && kc <= 0x95u) {
        uint32_t ws = (uint32_t)(kc - 0x90u);
        if (ws == 5u) ws = 0u;
        if (ws < WM_MAX_WORKSPACES) {
            wm_switch_workspace(c, st, ws);
        }
        return;
    }

    if (kc >= 0xA0u && kc <= 0xA5u) {
        uint32_t ws = (uint32_t)(kc - 0xA0u);
        if (ws == 5u) ws = 0u;
        if (ws < WM_MAX_WORKSPACES) {
            wm_move_focused_to_ws(c, st, ws);
        }
        return;
    }

    if (kc == 0xA8u) {
        (void)wm_spawn_app_by_name("term");
        return;
    }

    if (kc == 0xA9u) {
        wm_close_focused(c, st);
        return;
    }

    if (kc == 0xAAu) {
        (void)wm_spawn_app_by_name("explorer");
        return;
    }

    if (kc == 0xABu) {
        (void)wm_spawn_app_by_name("launcher");
        return;
    }

    if (kc == 0xACu) {
        wm_toggle_floating(c, st);
        return;
    }

    if (kc == 0xADu) {
        int r = comp_wm_exit(c);
        char tmp[64];
        (void)snprintf(tmp, sizeof(tmp), "axwm: exit cmd r=%d\n", r);
        dbg_write(tmp);
        return;
    }

    if (kc == 0xB1u) {
        wm_focus_next(c, st, -1);
        return;
    }
    if (kc == 0xB2u) {
        wm_focus_next(c, st, +1);
        return;
    }
    if (kc == 0xB3u) {
        wm_focus_next(c, st, -1);
        return;
    }
    if (kc == 0xB4u) {
        wm_focus_next(c, st, +1);
        return;
    }
}

int wm_handle_event(comp_conn_t* c, wm_state_t* st, const comp_ipc_wm_event_t* ev) {
    if (!c || !st || !ev) return 0;

    if (ev->kind == COMP_WM_EVENT_MAP) {
        wm_on_map(c, st, ev);
    } else if (ev->kind == COMP_WM_EVENT_UNMAP) {
        wm_on_unmap(c, st, ev);
        wm_apply_layout(c, st);
    } else if (ev->kind == COMP_WM_EVENT_COMMIT) {
        wm_on_commit(c, st, ev);
    } else if (ev->kind == COMP_WM_EVENT_CLICK) {
        wm_on_click(c, st, ev);
    } else if (ev->kind == COMP_WM_EVENT_KEY) {
        wm_on_key(c, st, ev);
    } else if (ev->kind == COMP_WM_EVENT_POINTER) {
        wm_on_pointer(c, st, ev);
    }

    return 0;
}
