#include "axwm_internal.h"

void wm_focus_next(comp_conn_t* c, wm_state_t* st, int dir) {
    if (!c || !st) return;
    if (dir == 0) return;

    int start = st->focused_idx;
    if (start < 0 || start >= WM_MAX_VIEWS) start = 0;

    for (int step = 1; step <= WM_MAX_VIEWS; step++) {
        int idx = (start + dir * step) % WM_MAX_VIEWS;
        if (idx < 0) idx += WM_MAX_VIEWS;
        if (wm_is_view_visible_on_active_ws(st, &st->views[idx]) && !st->views[idx].ui) {
            wm_focus_view_idx(c, st, idx);
            return;
        }
    }
}

void wm_switch_workspace(comp_conn_t* c, wm_state_t* st, uint32_t ws) {
    if (!c || !st) return;
    if (ws >= WM_MAX_WORKSPACES) return;
    if (st->active_ws == ws) return;
    st->active_ws = ws;

    for (int i = 0; i < WM_MAX_VIEWS; i++) {
        wm_view_t* v = &st->views[i];
        if (!v->mapped) continue;
        if (v->ui) continue;
        if (v->workspace == st->active_ws) {
            wm_show_view(c, v);
        } else {
            wm_hide_view(c, v);
        }
    }

    wm_clear_focus(st);
    for (int i = 0; i < WM_MAX_VIEWS; i++) {
        if (wm_is_view_visible_on_active_ws(st, &st->views[i]) && !st->views[i].ui) {
            wm_focus_view_idx(c, st, i);
            break;
        }
    }

    if (st->master_surface_id[st->active_ws] == 0) {
        wm_reselect_master_for_ws(st, st->active_ws);
    }
    wm_apply_layout(c, st);
    wm_ui_draw_bar(st);
    wm_ui_raise_and_place(c, st);
}

void wm_move_focused_to_ws(comp_conn_t* c, wm_state_t* st, uint32_t ws) {
    if (!c || !st) return;
    if (ws >= WM_MAX_WORKSPACES) return;
    int idx = st->focused_idx;
    if (idx < 0 || idx >= WM_MAX_VIEWS) return;

    wm_view_t* v = &st->views[idx];
    if (!wm_is_view_visible_on_active_ws(st, v)) return;

    uint32_t old_ws = v->workspace;
    if (!v->floating) {
        wm_layout_remove_view(st, old_ws, idx);
    }
    if (wm_master_matches(st, old_ws, v)) {
        wm_master_clear_for_ws(st, old_ws);
    }

    v->workspace = ws;
    if (st->master_surface_id[ws] == 0 && !v->floating) {
        wm_master_set_for_ws(st, ws, v->client_id, v->surface_id);
    }
    if (!v->floating) {
        if (st->layout_root[ws] < 0) {
            int n = wm_layout_alloc_node(st, ws);
            if (n >= 0) {
                st->layout_nodes[n].is_split = 0;
                st->layout_nodes[n].view_idx = idx;
                st->layout_root[ws] = n;
            }
        } else {
            int leaf = wm_layout_find_any_leaf(st, ws);
            if (leaf >= 0) {
                int split_on = st->layout_nodes[leaf].view_idx;
                if (split_on >= 0 && split_on < WM_MAX_VIEWS && split_on != idx) {
                    wm_layout_insert_split(st, ws, split_on, idx);
                }
            }
        }
    }

    if (ws != st->active_ws) {
        wm_hide_view(c, v);
        wm_clear_focus(st);
        for (int i = 0; i < WM_MAX_VIEWS; i++) {
            if (wm_is_view_visible_on_active_ws(st, &st->views[i]) && !st->views[i].ui) {
                wm_focus_view_idx(c, st, i);
                break;
            }
        }
    }

    if (old_ws != ws && st->master_surface_id[old_ws] == 0) {
        wm_reselect_master_for_ws(st, old_ws);
    }
    wm_apply_layout(c, st);
}

void wm_toggle_floating(comp_conn_t* c, wm_state_t* st) {
    if (!c || !st) return;
    int idx = st->focused_idx;
    if (idx < 0 || idx >= WM_MAX_VIEWS) return;

    wm_view_t* v = &st->views[idx];
    if (!wm_is_view_visible_on_active_ws(st, v)) return;
    v->floating = !v->floating;
    if (v->floating) {
        wm_layout_remove_view(st, v->workspace, idx);
    } else {
        uint32_t ws = v->workspace;
        if (ws < WM_MAX_WORKSPACES) {
            if (st->layout_root[ws] < 0) {
                int n = wm_layout_alloc_node(st, ws);
                if (n >= 0) {
                    st->layout_nodes[n].is_split = 0;
                    st->layout_nodes[n].view_idx = idx;
                    st->layout_root[ws] = n;
                }
            } else {
                int leaf = wm_layout_find_any_leaf(st, ws);
                if (leaf >= 0) {
                    int split_on = st->layout_nodes[leaf].view_idx;
                    if (split_on >= 0 && split_on < WM_MAX_VIEWS && split_on != idx) {
                        wm_layout_insert_split(st, ws, split_on, idx);
                    }
                }
            }
        }
    }
    wm_apply_layout(c, st);
}

void wm_move_focused_float(comp_conn_t* c, wm_state_t* st, int32_t dx, int32_t dy) {
    if (!c || !st) return;
    int idx = st->focused_idx;
    if (idx < 0 || idx >= WM_MAX_VIEWS) return;
    wm_view_t* v = &st->views[idx];
    if (!wm_is_view_visible_on_active_ws(st, v)) return;
    if (!v->floating) return;

    v->x += dx;
    v->y += dy;
    (void)comp_wm_move(c, v->client_id, v->surface_id, v->x, v->y);
}

void wm_close_focused(comp_conn_t* c, wm_state_t* st) {
    if (!c || !st) return;
    int idx = st->focused_idx;
    if (idx < 0 || idx >= WM_MAX_VIEWS) return;
    wm_view_t* v = &st->views[idx];
    if (!wm_is_view_visible_on_active_ws(st, v)) return;

    const uint32_t closing_client_id = v->client_id;
    const uint32_t closing_surface_id = v->surface_id;
    const int next_idx = wm_pick_next_focus_idx(st, idx);

    wm_request_close(c, st, closing_client_id, closing_surface_id);
    if (next_idx >= 0) {
        wm_focus_view_idx(c, st, next_idx);
    }
}

void wm_request_close(comp_conn_t* c, wm_state_t* st, uint32_t client_id, uint32_t surface_id) {
    if (!st) return;
    if (!c || comp_wm_close(c, client_id, surface_id) != 0) {
        st->pending_close = 1;
        st->pending_close_client_id = client_id;
        st->pending_close_surface_id = surface_id;
    }
}

void wm_request_exit(comp_conn_t* c, wm_state_t* st) {
    if (!st) return;
    if (!c || comp_wm_exit(c) != 0) {
        st->pending_exit = 1;
    }
}

void wm_flush_pending_cmds(comp_conn_t* c, wm_state_t* st) {
    if (!c || !st) return;
    if (st->pending_exit) {
        if (comp_wm_exit(c) == 0) {
            st->pending_exit = 0;
        }
    }
    if (st->pending_close) {
        if (comp_wm_close(c, st->pending_close_client_id, st->pending_close_surface_id) == 0) {
            st->pending_close = 0;
        }
    }
}
