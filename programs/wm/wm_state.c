#include "wm_internal.h"

void wm_reset_session_state(wm_state_t* st) {
    if (!st) return;
    for (int i = 0; i < WM_MAX_VIEWS; i++) {
        memset(&st->views[i], 0, sizeof(st->views[i]));
    }
    for (uint32_t ws = 0; ws < WM_MAX_WORKSPACES; ws++) {
        st->master_client_id[ws] = COMP_WM_CLIENT_NONE;
        st->master_surface_id[ws] = 0;
        st->layout_root[ws] = -1;
    }
    for (int i = 0; i < WM_MAX_LAYOUT_NODES; i++) {
        st->layout_nodes[i].used = 0;
        st->layout_nodes[i].workspace = 0;
        st->layout_nodes[i].parent = -1;
        st->layout_nodes[i].a = -1;
        st->layout_nodes[i].b = -1;
        st->layout_nodes[i].is_split = 0;
        st->layout_nodes[i].split_dir = WM_SPLIT_VERTICAL;
        st->layout_nodes[i].view_idx = -1;
    }
    st->focused_idx = -1;
    st->super_down = 0;
    st->pointer_buttons = 0;
    st->pointer_x = 0;
    st->pointer_y = 0;
    st->drag_active = 0;
    st->drag_view_idx = -1;
    st->drag_off_x = 0;
    st->drag_off_y = 0;
    st->drag_start_px = 0;
    st->drag_start_py = 0;
    st->drag_button_mask = 0;
    st->drag_requires_super = 0;
    st->drag_is_resize = 0;
    st->drag_resize_edges = 0;
    st->drag_resize_start_x = 0;
    st->drag_resize_start_y = 0;
    st->drag_resize_start_w = 0;
    st->drag_resize_start_h = 0;
    st->drag_resize_new_x = 0;
    st->drag_resize_new_y = 0;
    st->drag_resize_new_w = 0;
    st->drag_resize_new_h = 0;
    st->drag_preview_last_w = 0;
    st->drag_preview_last_h = 0;
    wm_ui_cleanup(&st->ui);

    st->run_mode = 0;
    st->run_len = 0;
    st->run_buf[0] = '\0';

    st->ui.client_id = COMP_WM_CLIENT_NONE;
    st->ui.surface_id = WM_UI_BAR_SURFACE_ID;
    st->ui.shm_fd = -1;
}

int wm_read_fb_info(uint32_t* out_w, uint32_t* out_h) {
    if (!out_w || !out_h) return -1;
    *out_w = 0;
    *out_h = 0;

    int fd_fb = open("/dev/fb0", 0);
    if (fd_fb < 0) return -1;

    fb_info_t info;
    int r = read(fd_fb, &info, sizeof(info));
    close(fd_fb);
    if (r < (int)sizeof(info)) return -1;
    if (info.width == 0 || info.height == 0) return -1;
    *out_w = info.width;
    *out_h = info.height;
    return 0;
}
