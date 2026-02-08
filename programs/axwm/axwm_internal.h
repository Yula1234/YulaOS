#pragma once

#include <yula.h>
#include <comp.h>
#include <font.h>

#define WM_MAX_VIEWS 64
#define WM_MAX_WORKSPACES 5u

#define WM_MAX_LAYOUT_NODES 128

#define WM_SPLIT_VERTICAL   0
#define WM_SPLIT_HORIZONTAL 1

#define WM_UI_BAR_SURFACE_ID 0x80000001u
#define WM_UI_BAR_H 28u

#define WM_RESIZE_EDGE_LEFT   1u
#define WM_RESIZE_EDGE_RIGHT  2u
#define WM_RESIZE_EDGE_TOP    4u
#define WM_RESIZE_EDGE_BOTTOM 8u

#define WM_RESIZE_HIT_PX 10
#define WM_RESIZE_MIN_W 240u
#define WM_RESIZE_MIN_H 160u

typedef struct {
    uint32_t client_id;
    uint32_t surface_id;
    uint32_t workspace;
    int mapped;
    int floating;
    int focused;
    int hidden;
    int ui;
    int32_t x;
    int32_t y;
    uint32_t w;
    uint32_t h;
    int32_t last_x;
    int32_t last_y;
} wm_view_t;

typedef struct {
    comp_conn_t c;
    int connected;

    uint32_t client_id;
    uint32_t surface_id;

    int shm_fd;
    char shm_name[32];
    uint32_t* pixels;
    uint32_t w;
    uint32_t h;
    uint32_t size_bytes;
} wm_ui_t;

typedef struct {
    wm_view_t views[WM_MAX_VIEWS];
    uint32_t active_ws;
    int focused_idx;
    uint32_t master_client_id[WM_MAX_WORKSPACES];
    uint32_t master_surface_id[WM_MAX_WORKSPACES];

    struct {
        int used;
        uint32_t workspace;
        int parent;
        int a;
        int b;
        int is_split;
        int split_dir;
        int view_idx;
    } layout_nodes[WM_MAX_LAYOUT_NODES];
    int layout_root[WM_MAX_WORKSPACES];

    uint32_t screen_w;
    uint32_t screen_h;
    int have_screen;

    int32_t gap_outer;
    int32_t gap_inner;
    int32_t float_step;

    int super_down;
    uint32_t pointer_buttons;
    int32_t pointer_x;
    int32_t pointer_y;

    int drag_active;
    int drag_view_idx;
    int32_t drag_off_x;
    int32_t drag_off_y;
    int32_t drag_start_px;
    int32_t drag_start_py;
    uint32_t drag_button_mask;
    int drag_requires_super;

    int drag_is_resize;
    uint32_t drag_resize_edges;
    int32_t drag_resize_start_x;
    int32_t drag_resize_start_y;
    uint32_t drag_resize_start_w;
    uint32_t drag_resize_start_h;
    int32_t drag_resize_new_x;
    int32_t drag_resize_new_y;
    uint32_t drag_resize_new_w;
    uint32_t drag_resize_new_h;
    uint32_t drag_preview_last_w;
    uint32_t drag_preview_last_h;
    int pending_exit;
    int pending_close;
    uint32_t pending_close_client_id;
    uint32_t pending_close_surface_id;

    wm_ui_t ui;
} wm_state_t;

static inline void dbg_write(const char* s) {
    if (!s) return;
    write(1, s, (uint32_t)strlen(s));
}

void wm_reset_session_state(wm_state_t* st);

int wm_ui_init(wm_state_t* st);
void wm_ui_cleanup(wm_ui_t* ui);
void wm_ui_pump(wm_ui_t* ui);
void wm_ui_raise_and_place(comp_conn_t* wm_conn, wm_state_t* st);
void wm_ui_draw_bar(wm_state_t* st);
void wm_ui_handle_bar_click(comp_conn_t* c, wm_state_t* st, int32_t x);

int wm_spawn_app_by_name(const char* name);

int wm_read_fb_info(uint32_t* out_w, uint32_t* out_h);

int wm_find_view_idx(const wm_state_t* st, uint32_t client_id, uint32_t surface_id);
wm_view_t* wm_get_or_create_view(wm_state_t* st, uint32_t client_id, uint32_t surface_id);
void wm_drop_view(wm_state_t* st, int idx);

int wm_is_view_visible_on_active_ws(const wm_state_t* st, const wm_view_t* v);
void wm_clear_focus(wm_state_t* st);
void wm_focus_view_idx(comp_conn_t* c, wm_state_t* st, int idx);
int wm_pick_next_focus_idx(const wm_state_t* st, int start_idx);

void wm_hide_view(comp_conn_t* c, wm_view_t* v);
void wm_show_view(comp_conn_t* c, wm_view_t* v);

int wm_layout_alloc_node(wm_state_t* st, uint32_t ws);
int wm_layout_find_any_leaf(const wm_state_t* st, uint32_t ws);
void wm_layout_remove_view(wm_state_t* st, uint32_t ws, int view_idx);
void wm_layout_insert_split(wm_state_t* st, uint32_t ws, int old_view_idx, int new_view_idx);

void wm_master_clear_for_ws(wm_state_t* st, uint32_t ws);
void wm_master_set_for_ws(wm_state_t* st, uint32_t ws, uint32_t client_id, uint32_t surface_id);
int wm_master_matches(const wm_state_t* st, uint32_t ws, const wm_view_t* v);
void wm_reselect_master_for_ws(wm_state_t* st, uint32_t ws);

void wm_apply_layout(comp_conn_t* c, wm_state_t* st);

void wm_stop_drag(comp_conn_t* c, wm_state_t* st);
void wm_start_drag(comp_conn_t* c, wm_state_t* st, int idx, int32_t px, int32_t py, uint32_t button_mask, int requires_super);
uint32_t wm_resize_edges_for_point(const wm_view_t* v, int32_t px, int32_t py);
void wm_start_resize(comp_conn_t* c, wm_state_t* st, int idx, int32_t px, int32_t py, uint32_t button_mask, uint32_t edges);

void wm_focus_next(comp_conn_t* c, wm_state_t* st, int dir);
void wm_switch_workspace(comp_conn_t* c, wm_state_t* st, uint32_t ws);
void wm_move_focused_to_ws(comp_conn_t* c, wm_state_t* st, uint32_t ws);
void wm_toggle_floating(comp_conn_t* c, wm_state_t* st);
void wm_move_focused_float(comp_conn_t* c, wm_state_t* st, int32_t dx, int32_t dy);
void wm_close_focused(comp_conn_t* c, wm_state_t* st);
void wm_request_close(comp_conn_t* c, wm_state_t* st, uint32_t client_id, uint32_t surface_id);
void wm_request_exit(comp_conn_t* c, wm_state_t* st);
void wm_flush_pending_cmds(comp_conn_t* c, wm_state_t* st);

int wm_handle_event(comp_conn_t* c, wm_state_t* st, const comp_ipc_wm_event_t* ev);
