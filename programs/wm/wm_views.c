#include "wm_internal.h"

static int wm_view_match(const wm_view_t* v, uint32_t client_id, uint32_t surface_id) {
    return v && v->mapped && v->client_id == client_id && v->surface_id == surface_id;
}

int wm_find_view_idx(const wm_state_t* st, uint32_t client_id, uint32_t surface_id) {
    if (!st) return -1;
    for (int i = 0; i < WM_MAX_VIEWS; i++) {
        if (wm_view_match(&st->views[i], client_id, surface_id)) return i;
    }
    return -1;
}

static wm_view_t* wm_alloc_view(wm_state_t* st, uint32_t client_id, uint32_t surface_id) {
    if (!st || surface_id == 0) return 0;
    for (int i = 0; i < WM_MAX_VIEWS; i++) {
        wm_view_t* v = &st->views[i];
        if (!v->mapped) {
            memset(v, 0, sizeof(*v));
            v->client_id = client_id;
            v->surface_id = surface_id;
            v->workspace = st->active_ws;
            v->mapped = 1;
            v->floating = 0;
            v->focused = 0;
            v->hidden = 0;
            v->ui = 0;
            v->x = 0;
            v->y = 0;
            v->w = 0;
            v->h = 0;
            v->last_x = 0;
            v->last_y = 0;
            return v;
        }
    }
    return 0;
}

wm_view_t* wm_get_or_create_view(wm_state_t* st, uint32_t client_id, uint32_t surface_id) {
    if (!st || surface_id == 0) return 0;
    int idx = wm_find_view_idx(st, client_id, surface_id);
    if (idx >= 0) return &st->views[idx];
    return wm_alloc_view(st, client_id, surface_id);
}

void wm_drop_view(wm_state_t* st, int idx) {
    if (!st) return;
    if (idx < 0 || idx >= WM_MAX_VIEWS) return;
    memset(&st->views[idx], 0, sizeof(st->views[idx]));
}

void wm_clear_focus(wm_state_t* st) {
    if (!st) return;
    for (int i = 0; i < WM_MAX_VIEWS; i++) st->views[i].focused = 0;
    st->focused_idx = -1;
}

void wm_master_clear_for_ws(wm_state_t* st, uint32_t ws) {
    if (!st) return;
    if (ws >= WM_MAX_WORKSPACES) return;
    st->master_client_id[ws] = COMP_WM_CLIENT_NONE;
    st->master_surface_id[ws] = 0;
}

void wm_master_set_for_ws(wm_state_t* st, uint32_t ws, uint32_t client_id, uint32_t surface_id) {
    if (!st) return;
    if (ws >= WM_MAX_WORKSPACES) return;
    if (surface_id == 0) return;
    st->master_client_id[ws] = client_id;
    st->master_surface_id[ws] = surface_id;
}

int wm_master_matches(const wm_state_t* st, uint32_t ws, const wm_view_t* v) {
    if (!st || !v) return 0;
    if (ws >= WM_MAX_WORKSPACES) return 0;
    if (st->master_surface_id[ws] == 0) return 0;
    return v->client_id == st->master_client_id[ws] && v->surface_id == st->master_surface_id[ws];
}

void wm_reselect_master_for_ws(wm_state_t* st, uint32_t ws) {
    if (!st) return;
    if (ws >= WM_MAX_WORKSPACES) return;

    for (int i = 0; i < WM_MAX_VIEWS; i++) {
        wm_view_t* v = &st->views[i];
        if (!v->mapped) continue;
        if (v->workspace != ws) continue;
        if (v->floating) continue;
        wm_master_set_for_ws(st, ws, v->client_id, v->surface_id);
        return;
    }

    wm_master_clear_for_ws(st, ws);
}

int wm_is_view_visible_on_active_ws(const wm_state_t* st, const wm_view_t* v) {
    if (!st || !v || !v->mapped) return 0;
    if (v->ui) return 1;
    if (v->workspace != st->active_ws) return 0;
    if (v->hidden) return 0;
    return 1;
}

void wm_focus_view_idx(comp_conn_t* c, wm_state_t* st, int idx) {
    if (!c || !st) return;
    if (idx < 0 || idx >= WM_MAX_VIEWS) return;
    wm_view_t* v = &st->views[idx];
    if (!wm_is_view_visible_on_active_ws(st, v)) return;
    if (v->ui) return;

    wm_clear_focus(st);
    v->focused = 1;
    st->focused_idx = idx;
    (void)comp_wm_focus(c, v->client_id, v->surface_id);
    (void)comp_wm_raise(c, v->client_id, v->surface_id);
    wm_ui_draw_bar(st);
    wm_ui_raise_and_place(c, st);
}

void wm_hide_view(comp_conn_t* c, wm_view_t* v) {
    if (!c || !v || !v->mapped) return;
    if (v->ui) return;
    if (v->hidden) return;
    v->hidden = 1;
    v->last_x = v->x;
    v->last_y = v->y;
    v->x = -20000;
    v->y = -20000;
    (void)comp_wm_move(c, v->client_id, v->surface_id, v->x, v->y);
}

void wm_show_view(comp_conn_t* c, wm_view_t* v) {
    if (!c || !v || !v->mapped) return;
    if (v->ui) return;
    if (!v->hidden) return;
    v->hidden = 0;
    v->x = v->last_x;
    v->y = v->last_y;
    (void)comp_wm_move(c, v->client_id, v->surface_id, v->x, v->y);
}
