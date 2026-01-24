#include "wm_internal.h"

void wm_stop_drag(comp_conn_t* c, wm_state_t* st) {
    if (!c || !st) return;
    if (!st->drag_active) return;

    int idx = st->drag_view_idx;
    if (idx >= 0 && idx < WM_MAX_VIEWS) {
        wm_view_t* v = &st->views[idx];
        if (v->mapped && v->surface_id != 0) {
            if (st->drag_is_resize) {
                if (st->drag_resize_new_w > 0 && st->drag_resize_new_h > 0) {
                    (void)comp_wm_move(c, v->client_id, v->surface_id, st->drag_resize_new_x,
                                      st->drag_resize_new_y);
                    v->x = st->drag_resize_new_x;
                    v->y = st->drag_resize_new_y;
                    (void)comp_wm_resize(c, v->client_id, v->surface_id, (int32_t)st->drag_resize_new_w,
                                        (int32_t)st->drag_resize_new_h);
                }
                (void)comp_wm_preview_clear(c, v->client_id, v->surface_id);
            }
            (void)comp_wm_pointer_grab(c, v->client_id, v->surface_id, 0);
        }
    }

    st->drag_active = 0;
    st->drag_view_idx = -1;
    st->drag_button_mask = 0;
    st->drag_requires_super = 0;
    st->drag_is_resize = 0;
    st->drag_resize_edges = 0;
    st->drag_preview_last_w = 0;
    st->drag_preview_last_h = 0;
}

void wm_start_drag(comp_conn_t* c, wm_state_t* st, int idx, int32_t px, int32_t py, uint32_t button_mask,
                  int requires_super) {
    if (!c || !st) return;
    if (idx < 0 || idx >= WM_MAX_VIEWS) return;
    wm_view_t* v = &st->views[idx];
    if (v->ui) return;
    if (!wm_is_view_visible_on_active_ws(st, v)) return;

    if (!v->floating) {
        v->floating = 1;
        wm_layout_remove_view(st, v->workspace, idx);
        wm_apply_layout(c, st);
    }

    st->drag_active = 1;
    st->drag_view_idx = idx;
    st->drag_off_x = px - v->x;
    st->drag_off_y = py - v->y;
    st->drag_start_px = px;
    st->drag_start_py = py;
    st->drag_button_mask = button_mask;
    st->drag_requires_super = requires_super ? 1 : 0;
    st->drag_is_resize = 0;
    st->drag_resize_edges = 0;
    (void)comp_wm_pointer_grab(c, v->client_id, v->surface_id, 1);
}

uint32_t wm_resize_edges_for_point(const wm_view_t* v, int32_t px, int32_t py) {
    if (!v) return 0;
    if (v->w == 0 || v->h == 0) return 0;
    int32_t lx = px - v->x;
    int32_t ly = py - v->y;
    if (lx < 0 || ly < 0) return 0;
    if ((uint32_t)lx >= v->w || (uint32_t)ly >= v->h) return 0;

    uint32_t edges = 0;
    if (lx < WM_RESIZE_HIT_PX) edges |= WM_RESIZE_EDGE_LEFT;
    if (lx >= (int32_t)v->w - WM_RESIZE_HIT_PX) edges |= WM_RESIZE_EDGE_RIGHT;
    if (ly < WM_RESIZE_HIT_PX) edges |= WM_RESIZE_EDGE_TOP;
    if (ly >= (int32_t)v->h - WM_RESIZE_HIT_PX) edges |= WM_RESIZE_EDGE_BOTTOM;
    return edges;
}

void wm_start_resize(comp_conn_t* c, wm_state_t* st, int idx, int32_t px, int32_t py, uint32_t button_mask,
                     uint32_t edges) {
    if (!c || !st) return;
    if (idx < 0 || idx >= WM_MAX_VIEWS) return;
    if (edges == 0) return;

    wm_view_t* v = &st->views[idx];
    if (v->ui) return;
    if (!wm_is_view_visible_on_active_ws(st, v)) return;

    if (!v->floating) {
        v->floating = 1;
        wm_layout_remove_view(st, v->workspace, idx);
        wm_apply_layout(c, st);
    }

    st->drag_active = 1;
    st->drag_view_idx = idx;
    st->drag_off_x = 0;
    st->drag_off_y = 0;
    st->drag_start_px = px;
    st->drag_start_py = py;
    st->drag_button_mask = button_mask;
    st->drag_requires_super = 0;

    st->drag_is_resize = 1;
    st->drag_resize_edges = edges;
    st->drag_resize_start_x = v->x;
    st->drag_resize_start_y = v->y;
    st->drag_resize_start_w = v->w;
    st->drag_resize_start_h = v->h;
    st->drag_resize_new_x = v->x;
    st->drag_resize_new_y = v->y;
    st->drag_resize_new_w = v->w;
    st->drag_resize_new_h = v->h;
    st->drag_preview_last_w = 0;
    st->drag_preview_last_h = 0;

    (void)comp_wm_pointer_grab(c, v->client_id, v->surface_id, 1);
}
