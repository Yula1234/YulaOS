#include "wm_internal.h"

static inline int32_t wm_max_i32(int32_t a, int32_t b) {
    return a > b ? a : b;
}

int wm_layout_alloc_node(wm_state_t* st, uint32_t ws) {
    if (!st) return -1;
    for (int i = 0; i < WM_MAX_LAYOUT_NODES; i++) {
        if (!st->layout_nodes[i].used) {
            st->layout_nodes[i].used = 1;
            st->layout_nodes[i].workspace = ws;
            st->layout_nodes[i].parent = -1;
            st->layout_nodes[i].a = -1;
            st->layout_nodes[i].b = -1;
            st->layout_nodes[i].is_split = 0;
            st->layout_nodes[i].split_dir = WM_SPLIT_VERTICAL;
            st->layout_nodes[i].view_idx = -1;
            return i;
        }
    }
    return -1;
}

static void wm_layout_free_node(wm_state_t* st, int n) {
    if (!st) return;
    if (n < 0 || n >= WM_MAX_LAYOUT_NODES) return;
    st->layout_nodes[n].used = 0;
    st->layout_nodes[n].workspace = 0;
    st->layout_nodes[n].parent = -1;
    st->layout_nodes[n].a = -1;
    st->layout_nodes[n].b = -1;
    st->layout_nodes[n].is_split = 0;
    st->layout_nodes[n].split_dir = WM_SPLIT_VERTICAL;
    st->layout_nodes[n].view_idx = -1;
}

static int wm_layout_find_leaf_for_view(const wm_state_t* st, uint32_t ws, int view_idx) {
    if (!st) return -1;
    if (ws >= WM_MAX_WORKSPACES) return -1;
    if (view_idx < 0 || view_idx >= WM_MAX_VIEWS) return -1;
    for (int i = 0; i < WM_MAX_LAYOUT_NODES; i++) {
        if (!st->layout_nodes[i].used) continue;
        if (st->layout_nodes[i].workspace != ws) continue;
        if (st->layout_nodes[i].is_split) continue;
        if (st->layout_nodes[i].view_idx == view_idx) return i;
    }
    return -1;
}

int wm_layout_find_any_leaf(const wm_state_t* st, uint32_t ws) {
    if (!st) return -1;
    if (ws >= WM_MAX_WORKSPACES) return -1;
    for (int i = 0; i < WM_MAX_LAYOUT_NODES; i++) {
        if (!st->layout_nodes[i].used) continue;
        if (st->layout_nodes[i].workspace != ws) continue;
        if (!st->layout_nodes[i].is_split && st->layout_nodes[i].view_idx >= 0) return i;
    }
    return -1;
}

static int wm_layout_pick_split_dir(const wm_state_t* st, int view_idx) {
    if (!st) return WM_SPLIT_VERTICAL;
    if (view_idx < 0 || view_idx >= WM_MAX_VIEWS) return WM_SPLIT_VERTICAL;
    const wm_view_t* v = &st->views[view_idx];
    uint32_t w = v->w;
    uint32_t h = v->h;
    if (w == 0 || h == 0) {
        w = st->screen_w;
        h = st->screen_h;
    }
    return (w >= h) ? WM_SPLIT_VERTICAL : WM_SPLIT_HORIZONTAL;
}

void wm_layout_remove_view(wm_state_t* st, uint32_t ws, int view_idx) {
    if (!st) return;
    if (ws >= WM_MAX_WORKSPACES) return;
    int leaf = wm_layout_find_leaf_for_view(st, ws, view_idx);
    if (leaf < 0) return;

    int parent = st->layout_nodes[leaf].parent;
    if (parent < 0) {
        wm_layout_free_node(st, leaf);
        st->layout_root[ws] = -1;
        return;
    }

    int sibling = (st->layout_nodes[parent].a == leaf) ? st->layout_nodes[parent].b : st->layout_nodes[parent].a;
    int grand = st->layout_nodes[parent].parent;

    if (grand < 0) {
        st->layout_root[ws] = sibling;
        if (sibling >= 0) st->layout_nodes[sibling].parent = -1;
    } else {
        if (st->layout_nodes[grand].a == parent) {
            st->layout_nodes[grand].a = sibling;
        } else if (st->layout_nodes[grand].b == parent) {
            st->layout_nodes[grand].b = sibling;
        }
        if (sibling >= 0) st->layout_nodes[sibling].parent = grand;
    }

    wm_layout_free_node(st, leaf);
    wm_layout_free_node(st, parent);
}

void wm_layout_insert_split(wm_state_t* st, uint32_t ws, int old_view_idx, int new_view_idx) {
    if (!st) return;
    if (ws >= WM_MAX_WORKSPACES) return;
    if (old_view_idx < 0 || old_view_idx >= WM_MAX_VIEWS) return;
    if (new_view_idx < 0 || new_view_idx >= WM_MAX_VIEWS) return;

    int leaf = wm_layout_find_leaf_for_view(st, ws, old_view_idx);
    if (leaf < 0) {
        leaf = wm_layout_find_any_leaf(st, ws);
    }
    if (leaf < 0) {
        int n = wm_layout_alloc_node(st, ws);
        if (n < 0) return;
        st->layout_nodes[n].is_split = 0;
        st->layout_nodes[n].view_idx = new_view_idx;
        st->layout_nodes[n].parent = -1;
        st->layout_root[ws] = n;
        return;
    }

    int a = wm_layout_alloc_node(st, ws);
    int b = wm_layout_alloc_node(st, ws);
    if (a < 0 || b < 0) {
        if (a >= 0) wm_layout_free_node(st, a);
        if (b >= 0) wm_layout_free_node(st, b);
        return;
    }

    st->layout_nodes[a].is_split = 0;
    st->layout_nodes[a].view_idx = old_view_idx;
    st->layout_nodes[a].parent = leaf;

    st->layout_nodes[b].is_split = 0;
    st->layout_nodes[b].view_idx = new_view_idx;
    st->layout_nodes[b].parent = leaf;

    st->layout_nodes[leaf].is_split = 1;
    st->layout_nodes[leaf].view_idx = -1;
    st->layout_nodes[leaf].a = a;
    st->layout_nodes[leaf].b = b;
    st->layout_nodes[leaf].split_dir = wm_layout_pick_split_dir(st, old_view_idx);
}

void wm_apply_layout(comp_conn_t* c, wm_state_t* st) {
    if (!c || !st) return;

    if (!st->have_screen) {
        uint32_t sw = 0, sh = 0;
        if (wm_read_fb_info(&sw, &sh) == 0) {
            st->screen_w = sw;
            st->screen_h = sh;
            st->have_screen = 1;
        }
    }

    for (int i = 0; i < WM_MAX_VIEWS; i++) {
        wm_view_t* v = &st->views[i];
        if (!v->mapped) continue;
        if (v->ui) continue;
        if (v->workspace != st->active_ws) {
            wm_hide_view(c, v);
            continue;
        }
        if (v->hidden) wm_show_view(c, v);
    }

    const int32_t bar_h = (st->ui.client_id != COMP_WM_CLIENT_NONE) ? (int32_t)st->ui.h : 0;
    int32_t ax = st->gap_outer;
    int32_t ay = st->gap_outer + bar_h;
    int32_t aw = (int32_t)st->screen_w - 2 * st->gap_outer;
    int32_t ah = (int32_t)st->screen_h - 2 * st->gap_outer - bar_h;
    if (aw <= 0 || ah <= 0) {
        wm_ui_raise_and_place(c, st);
        return;
    }

    uint32_t ws = st->active_ws;
    int root = (ws < WM_MAX_WORKSPACES) ? st->layout_root[ws] : -1;

    if (root >= 0) {
        typedef struct {
            int32_t x, y, w, h;
        } wm_rect_i32_t;

        wm_rect_i32_t stack_nodes[WM_MAX_LAYOUT_NODES];
        int stack_idx[WM_MAX_LAYOUT_NODES];
        int sp = 0;

        stack_idx[sp] = root;
        stack_nodes[sp] = (wm_rect_i32_t){ax, ay, aw, ah};
        sp++;

        while (sp > 0) {
            sp--;
            int n = stack_idx[sp];
            wm_rect_i32_t r = stack_nodes[sp];

            if (n < 0 || n >= WM_MAX_LAYOUT_NODES) continue;
            if (!st->layout_nodes[n].used) continue;
            if (st->layout_nodes[n].workspace != ws) continue;

            if (!st->layout_nodes[n].is_split) {
                int vidx = st->layout_nodes[n].view_idx;
                if (vidx < 0 || vidx >= WM_MAX_VIEWS) continue;
                wm_view_t* v = &st->views[vidx];
                if (!v->mapped || v->ui || v->workspace != ws) continue;
                if (v->floating) continue;

                int32_t nx = r.x;
                int32_t ny = r.y;
                int32_t nw = r.w;
                int32_t nh = r.h;
                if (nw <= 0 || nh <= 0) continue;

                if (nw < (int32_t)WM_RESIZE_MIN_W) nw = (int32_t)WM_RESIZE_MIN_W;
                if (nh < (int32_t)WM_RESIZE_MIN_H) nh = (int32_t)WM_RESIZE_MIN_H;

                const int need_move = (v->x != nx) || (v->y != ny);
                const int need_resize = ((int32_t)v->w != nw) || ((int32_t)v->h != nh);
                if (need_resize) {
                    (void)comp_wm_resize(c, v->client_id, v->surface_id, nw, nh);
                    v->w = (uint32_t)nw;
                    v->h = (uint32_t)nh;
                }
                if (need_move) {
                    (void)comp_wm_move(c, v->client_id, v->surface_id, nx, ny);
                    v->x = nx;
                    v->y = ny;
                }
                continue;
            }

            int a = st->layout_nodes[n].a;
            int b = st->layout_nodes[n].b;
            if (a < 0 || b < 0) continue;

            if (st->layout_nodes[n].split_dir == WM_SPLIT_VERTICAL) {
                int32_t gap = st->gap_inner;
                if (gap < 0) gap = 0;
                int32_t left_w = (r.w - gap) / 2;
                int32_t right_w = (r.w - gap) - left_w;
                left_w = wm_max_i32(0, left_w);
                right_w = wm_max_i32(0, right_w);

                wm_rect_i32_t ra = (wm_rect_i32_t){r.x, r.y, left_w, r.h};
                wm_rect_i32_t rb = (wm_rect_i32_t){r.x + left_w + gap, r.y, right_w, r.h};

                stack_idx[sp] = b;
                stack_nodes[sp] = rb;
                sp++;
                stack_idx[sp] = a;
                stack_nodes[sp] = ra;
                sp++;
            } else {
                int32_t gap = st->gap_inner;
                if (gap < 0) gap = 0;
                int32_t top_h = (r.h - gap) / 2;
                int32_t bot_h = (r.h - gap) - top_h;
                top_h = wm_max_i32(0, top_h);
                bot_h = wm_max_i32(0, bot_h);

                wm_rect_i32_t ra = (wm_rect_i32_t){r.x, r.y, r.w, top_h};
                wm_rect_i32_t rb = (wm_rect_i32_t){r.x, r.y + top_h + gap, r.w, bot_h};

                stack_idx[sp] = b;
                stack_nodes[sp] = rb;
                sp++;
                stack_idx[sp] = a;
                stack_nodes[sp] = ra;
                sp++;
            }
        }
    }

    wm_ui_raise_and_place(c, st);
}
