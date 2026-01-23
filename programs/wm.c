// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#include <yula.h>
#include <comp.h>
#include <font.h>

static volatile int g_should_exit;

#define WM_MAX_VIEWS 64
#define WM_MAX_WORKSPACES 5u

#define WM_UI_BAR_SURFACE_ID 0x80000001u
#define WM_UI_BAR_H 20u

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

    wm_ui_t ui;

    int run_mode;
    char run_buf[32];
    int run_len;
} wm_state_t;

static void wm_apply_layout(comp_conn_t* c, wm_state_t* st);
static int wm_read_fb_info(uint32_t* out_w, uint32_t* out_h);
static int wm_is_view_visible_on_active_ws(const wm_state_t* st, const wm_view_t* v);

static void on_signal(int sig) {
    (void)sig;
    g_should_exit = 1;
    sigreturn();
    for (;;) {
    }
}

static inline void dbg_write(const char* s) {
    if (!s) return;
    write(1, s, (uint32_t)strlen(s));
}

static void wm_ui_cleanup(wm_ui_t* ui);

static void wm_reset_session_state(wm_state_t* st) {
    if (!st) return;
    for (int i = 0; i < WM_MAX_VIEWS; i++) {
        memset(&st->views[i], 0, sizeof(st->views[i]));
    }
    for (uint32_t ws = 0; ws < WM_MAX_WORKSPACES; ws++) {
        st->master_client_id[ws] = COMP_WM_CLIENT_NONE;
        st->master_surface_id[ws] = 0;
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

static void wm_ui_cleanup(wm_ui_t* ui) {
    if (!ui) return;

    if (ui->c.connected && ui->surface_id) {
        (void)comp_send_destroy_surface(&ui->c, ui->surface_id, 0u);
    }

    if (ui->pixels && ui->size_bytes) {
        (void)munmap((void*)ui->pixels, ui->size_bytes);
    }
    ui->pixels = 0;

    if (ui->shm_fd >= 0) {
        close(ui->shm_fd);
        ui->shm_fd = -1;
    }

    if (ui->shm_name[0]) {
        (void)shm_unlink_named(ui->shm_name);
        ui->shm_name[0] = '\0';
    }

    if (ui->c.connected) {
        comp_disconnect(&ui->c);
    } else {
        comp_conn_reset(&ui->c);
    }
    ui->connected = 0;
    ui->client_id = COMP_WM_CLIENT_NONE;
    ui->surface_id = 0;
    ui->w = 0;
    ui->h = 0;
    ui->size_bytes = 0;
}

static void wm_ui_fill(uint32_t* buf, uint32_t w, uint32_t h, uint32_t color) {
    if (!buf || w == 0 || h == 0) return;
    const uint32_t n = w * h;
    for (uint32_t i = 0; i < n; i++) {
        buf[i] = color;
    }
}

static void wm_ui_pump(wm_ui_t* ui) {
    if (!ui || !ui->connected) return;
    comp_ipc_hdr_t hdr;
    uint8_t payload[COMP_IPC_MAX_PAYLOAD];
    for (;;) {
        int r = comp_try_recv(&ui->c, &hdr, payload, (uint32_t)sizeof(payload));
        if (r < 0) {
            wm_ui_cleanup(ui);
            return;
        }
        if (r == 0) break;
    }
}

static void wm_ui_raise_and_place(comp_conn_t* wm_conn, wm_state_t* st) {
    if (!wm_conn || !st) return;
    if (st->ui.client_id == COMP_WM_CLIENT_NONE || st->ui.surface_id == 0) return;
    (void)comp_wm_move(wm_conn, st->ui.client_id, st->ui.surface_id, 0, 0);
    (void)comp_wm_raise(wm_conn, st->ui.client_id, st->ui.surface_id);
}

static void wm_ui_draw_bar(wm_state_t* st);
static void wm_switch_workspace(comp_conn_t* c, wm_state_t* st, uint32_t ws);

static int wm_spawn_app_by_name(const char* name) {
    if (!name) return -1;
    size_t n = strlen(name);
    if (n == 0) return -1;

    const char* base = name;
    for (size_t i = 0; i < n; i++) {
        if (name[i] == '/') base = name + i + 1u;
    }

    char argv0[32];
    argv0[0] = '\0';
    {
        size_t bn = strlen(base);
        if (bn >= 4 && strcmp(base + (bn - 4u), ".exe") == 0) bn -= 4u;
        if (bn >= sizeof(argv0)) bn = sizeof(argv0) - 1u;
        memcpy(argv0, base, bn);
        argv0[bn] = '\0';
    }
    char* argv[1];
    argv[0] = argv0;

    if (name[0] == '/') {
        return spawn_process(name, 1, argv);
    }

    const int has_exe = (n >= 4 && strcmp(name + (n - 4u), ".exe") == 0);

    char path1[96];
    char path2[96];
    if (has_exe) {
        (void)snprintf(path1, sizeof(path1), "/bin/%s", name);
        (void)snprintf(path2, sizeof(path2), "/bin/usr/%s", name);
    } else {
        (void)snprintf(path1, sizeof(path1), "/bin/%s.exe", name);
        (void)snprintf(path2, sizeof(path2), "/bin/usr/%s.exe", name);
    }

    int pid = spawn_process(path1, 1, argv);
    if (pid < 0) {
        pid = spawn_process(path2, 1, argv);
    }

    {
        char tmp[128];
        (void)snprintf(tmp, sizeof(tmp), "wm: spawn name='%s' pid=%d\n", name, pid);
        dbg_write(tmp);
    }
    return pid;
}

static int wm_ui_bar_run_hit(int32_t x) {
    const int32_t base_x = 6;
    const int32_t slot_w = 12;
    const int32_t start_x = base_x + (int32_t)WM_MAX_WORKSPACES * slot_w + 14;
    const char* label = "Run";
    const int32_t w = (int32_t)strlen(label) * 8 + 12;
    return x >= start_x && x < start_x + w;
}

static int wm_ui_bar_launcher_pick(int32_t x) {
    const int32_t base_x = 6;
    const int32_t slot_w = 12;
    const int32_t run_w = (int32_t)strlen("Run") * 8 + 12;
    const int32_t start_x = base_x + (int32_t)WM_MAX_WORKSPACES * slot_w + 14 + run_w + 8;

    if (x < start_x) return -1;

    const char* labels[] = { "Paint", "Explorer", "GEditor" };
    const int n = (int)(sizeof(labels) / sizeof(labels[0]));

    int32_t bx = start_x;
    for (int i = 0; i < n; i++) {
        const int32_t w = (int32_t)strlen(labels[i]) * 8 + 12;
        if (x >= bx && x < bx + w) return i;
        bx += w + 8;
    }
    return -1;
}

static void wm_spawn_app(int idx) {
    char* argv[1];
    if (idx == 0) {
        argv[0] = (char*)"paint";
        const char* path = "/bin/paint.exe";
        int pid = spawn_process(path, 1, argv);
        if (pid < 0) {
            path = "/bin/usr/paint.exe";
            pid = spawn_process(path, 1, argv);
        }
        {
            char tmp[96];
            (void)snprintf(tmp, sizeof(tmp), "wm: spawn paint pid=%d path=%s\n", pid, path);
            dbg_write(tmp);
        }
    } else if (idx == 1) {
        argv[0] = (char*)"explorer";
        const char* path = "/bin/explorer.exe";
        int pid = spawn_process(path, 1, argv);
        if (pid < 0) {
            path = "/bin/usr/explorer.exe";
            pid = spawn_process(path, 1, argv);
        }
        {
            char tmp[96];
            (void)snprintf(tmp, sizeof(tmp), "wm: spawn explorer pid=%d path=%s\n", pid, path);
            dbg_write(tmp);
        }
    } else if (idx == 2) {
        argv[0] = (char*)"geditor";
        const char* path = "/bin/geditor.exe";
        int pid = spawn_process(path, 1, argv);
        if (pid < 0) {
            path = "/bin/usr/geditor.exe";
            pid = spawn_process(path, 1, argv);
        }
        {
            char tmp[96];
            (void)snprintf(tmp, sizeof(tmp), "wm: spawn geditor pid=%d path=%s\n", pid, path);
            dbg_write(tmp);
        }
    }
}

static void wm_ui_handle_bar_click(comp_conn_t* c, wm_state_t* st, int32_t x) {
    if (!c || !st) return;

    if (x < 0) return;
    {
        const int32_t slot_w = 12;
        const int32_t base_x = 6;
        const int32_t rel = x - base_x;
        if (rel >= 0) {
            const uint32_t ws = (uint32_t)(rel / slot_w);
            if (ws < WM_MAX_WORKSPACES) {
                wm_switch_workspace(c, st, ws);
            }
        }
    }

    if (wm_ui_bar_run_hit(x)) {
        st->run_mode = !st->run_mode;
        st->run_len = 0;
        st->run_buf[0] = '\0';
        wm_ui_draw_bar(st);
        wm_ui_raise_and_place(c, st);
        return;
    }

    {
        const int app = wm_ui_bar_launcher_pick(x);
        {
            char tmp[64];
            (void)snprintf(tmp, sizeof(tmp), "wm: bar click x=%d app=%d\n", (int)x, app);
            dbg_write(tmp);
        }
        if (app >= 0) {
            wm_spawn_app(app);
        }
    }

    wm_ui_draw_bar(st);
    wm_ui_raise_and_place(c, st);
}

static void wm_ui_draw_bar(wm_state_t* st) {
    if (!st) return;
    wm_ui_t* ui = &st->ui;
    if (!ui->connected || !ui->c.connected || !ui->pixels) return;

    wm_ui_fill(ui->pixels, ui->w, ui->h, 0x202020u);

    if (ui->h) {
        uint32_t* row = ui->pixels + (ui->h - 1u) * ui->w;
        for (uint32_t x = 0; x < ui->w; x++) row[x] = 0x101010u;
    }

    int x = 6;
    for (uint32_t i = 0; i < WM_MAX_WORKSPACES; i++) {
        char tmp[2];
        tmp[0] = (char)('1' + (int)i);
        tmp[1] = '\0';
        const uint32_t col = (i == st->active_ws) ? 0xE0E0E0u : 0x808080u;
        draw_string(ui->pixels, (int)ui->w, (int)ui->h, x, 6, tmp, col);
        x += 12;
    }

    {
        int bx = 6 + (int)WM_MAX_WORKSPACES * 12 + 14;
        const uint32_t col = st->run_mode ? 0xE0E0E0u : 0xB8B8B8u;
        draw_string(ui->pixels, (int)ui->w, (int)ui->h, bx + 6, 6, "Run", col);
        bx += (int)strlen("Run") * 8 + 12 + 8;

        if (st->run_mode) {
            char tmp[64];
            (void)snprintf(tmp, sizeof(tmp), "> %s", st->run_buf);
            draw_string(ui->pixels, (int)ui->w, (int)ui->h, bx + 2, 6, tmp, 0xE0E0E0u);
        } else {
            const char* labels[] = { "Paint", "Explorer", "GEditor" };
            const int n = (int)(sizeof(labels) / sizeof(labels[0]));
            for (int i = 0; i < n; i++) {
                draw_string(ui->pixels, (int)ui->w, (int)ui->h, bx + 6, 6, labels[i], 0xB8B8B8u);
                bx += (int)strlen(labels[i]) * 8 + 12 + 8;
            }
        }
    }

    if (st->focused_idx >= 0 && st->focused_idx < WM_MAX_VIEWS) {
        const wm_view_t* v = &st->views[st->focused_idx];
        if (wm_is_view_visible_on_active_ws(st, v) && !v->ui) {
            char info[64];
            (void)snprintf(info, sizeof(info), "c%u:s%u", (unsigned)v->client_id, (unsigned)v->surface_id);
            int sx = (int)ui->w - ((int)strlen(info) * 8 + 6);
            if (sx < 0) sx = 0;
            draw_string(ui->pixels, (int)ui->w, (int)ui->h, sx, 6, info, 0xB8B8B8u);
        }
    }

    uint16_t err = 0;
    int r = comp_send_commit_sync(&ui->c, ui->surface_id, 0, 0, 0u, 500u, &err);
    if (r != 0) {
        char tmp[96];
        (void)snprintf(tmp, sizeof(tmp), "wm_ui: draw commit failed r=%d err=%u\n", r, (unsigned)err);
        dbg_write(tmp);
        wm_ui_cleanup(ui);
        return;
    }
    wm_ui_pump(ui);
}

static int wm_ui_init(wm_state_t* st) {
    if (!st) return -1;
    if (st->ui.connected) return 0;

    dbg_write("wm_ui: init\n");

    if (!st->have_screen) {
        uint32_t sw = 0, sh = 0;
        if (wm_read_fb_info(&sw, &sh) == 0) {
            st->screen_w = sw;
            st->screen_h = sh;
            st->have_screen = 1;
        }
    }
    if (!st->have_screen || st->screen_w == 0) {
        dbg_write("wm_ui: no screen\n");
        return -1;
    }

    wm_ui_t* ui = &st->ui;
    memset(ui, 0, sizeof(*ui));
    ui->client_id = COMP_WM_CLIENT_NONE;
    ui->surface_id = WM_UI_BAR_SURFACE_ID;
    ui->shm_fd = -1;
    ui->pixels = 0;
    ui->w = st->screen_w;
    ui->h = WM_UI_BAR_H;
    ui->size_bytes = ui->w * ui->h * 4u;

    const int pid = getpid();
    int created = 0;
    for (int i = 0; i < 8; i++) {
        (void)snprintf(ui->shm_name, sizeof(ui->shm_name), "wmbar_%d_%d", pid, i);
        ui->shm_fd = shm_create_named(ui->shm_name, ui->size_bytes);
        if (ui->shm_fd >= 0) {
            created = 1;
            break;
        }
    }
    if (!created) {
        dbg_write("wm_ui: shm_create_named failed\n");
        ui->shm_name[0] = '\0';
        ui->shm_fd = -1;
        return -1;
    }

    ui->pixels = (uint32_t*)mmap(ui->shm_fd, ui->size_bytes, MAP_SHARED);
    if (!ui->pixels) {
        dbg_write("wm_ui: mmap failed\n");
        close(ui->shm_fd);
        ui->shm_fd = -1;
        (void)shm_unlink_named(ui->shm_name);
        ui->shm_name[0] = '\0';
        return -1;
    }

    comp_conn_reset(&ui->c);
    if (comp_connect(&ui->c, "compositor") != 0) {
        dbg_write("wm_ui: ipc_connect compositor failed\n");
        wm_ui_cleanup(ui);
        return -1;
    }

    uint16_t err = 0;
    int r = comp_send_hello_sync(&ui->c, 2000u, &err);
    if (r != 0) {
        char tmp[96];
        (void)snprintf(tmp, sizeof(tmp), "wm_ui: hello failed r=%d err=%u\n", r, (unsigned)err);
        dbg_write(tmp);
        wm_ui_cleanup(ui);
        return -1;
    }

    err = 0;
    r = comp_send_attach_shm_name_sync(&ui->c, ui->surface_id, ui->shm_name, ui->size_bytes, ui->w, ui->h, ui->w, 0u, 2000u, &err);
    if (r != 0) {
        char tmp[96];
        (void)snprintf(tmp, sizeof(tmp), "wm_ui: attach failed r=%d err=%u\n", r, (unsigned)err);
        dbg_write(tmp);
        wm_ui_cleanup(ui);
        return -1;
    }

    err = 0;
    r = comp_send_commit_sync(&ui->c, ui->surface_id, 0, 0, 0u, 2000u, &err);
    if (r != 0) {
        char tmp[96];
        (void)snprintf(tmp, sizeof(tmp), "wm_ui: commit failed r=%d err=%u\n", r, (unsigned)err);
        dbg_write(tmp);
        wm_ui_cleanup(ui);
        return -1;
    }

    ui->connected = 1;
    dbg_write("wm_ui: ready\n");
    wm_ui_draw_bar(st);
    return 0;
}

static int wm_read_fb_info(uint32_t* out_w, uint32_t* out_h) {
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

static int wm_view_match(const wm_view_t* v, uint32_t client_id, uint32_t surface_id) {
    return v && v->mapped && v->client_id == client_id && v->surface_id == surface_id;
}

static int wm_find_view_idx(const wm_state_t* st, uint32_t client_id, uint32_t surface_id) {
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

static wm_view_t* wm_get_or_create_view(wm_state_t* st, uint32_t client_id, uint32_t surface_id) {
    if (!st || surface_id == 0) return 0;
    int idx = wm_find_view_idx(st, client_id, surface_id);
    if (idx >= 0) return &st->views[idx];
    return wm_alloc_view(st, client_id, surface_id);
}

static void wm_clear_focus(wm_state_t* st) {
    if (!st) return;
    for (int i = 0; i < WM_MAX_VIEWS; i++) st->views[i].focused = 0;
    st->focused_idx = -1;
}

static void wm_master_clear_for_ws(wm_state_t* st, uint32_t ws) {
    if (!st) return;
    if (ws >= WM_MAX_WORKSPACES) return;
    st->master_client_id[ws] = COMP_WM_CLIENT_NONE;
    st->master_surface_id[ws] = 0;
}

static void wm_master_set_for_ws(wm_state_t* st, uint32_t ws, uint32_t client_id, uint32_t surface_id) {
    if (!st) return;
    if (ws >= WM_MAX_WORKSPACES) return;
    if (surface_id == 0) return;
    st->master_client_id[ws] = client_id;
    st->master_surface_id[ws] = surface_id;
}

static int wm_master_matches(const wm_state_t* st, uint32_t ws, const wm_view_t* v) {
    if (!st || !v) return 0;
    if (ws >= WM_MAX_WORKSPACES) return 0;
    if (st->master_surface_id[ws] == 0) return 0;
    return v->client_id == st->master_client_id[ws] && v->surface_id == st->master_surface_id[ws];
}

static int wm_pick_master_idx(const wm_state_t* st, const int* tiled, int ntiled) {
    if (!st || !tiled || ntiled <= 0) return -1;

    uint32_t ws = st->active_ws;
    for (int i = 0; i < ntiled; i++) {
        const wm_view_t* v = &st->views[tiled[i]];
        if (wm_master_matches(st, ws, v)) return tiled[i];
    }
    return tiled[0];
}

static void wm_reselect_master_for_ws(wm_state_t* st, uint32_t ws) {
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

static int wm_is_view_visible_on_active_ws(const wm_state_t* st, const wm_view_t* v) {
    if (!st || !v || !v->mapped) return 0;
    if (v->ui) return 1;
    if (v->workspace != st->active_ws) return 0;
    if (v->hidden) return 0;
    return 1;
}

static void wm_focus_view_idx(comp_conn_t* c, wm_state_t* st, int idx) {
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

static void wm_stop_drag(comp_conn_t* c, wm_state_t* st) {
    if (!c || !st) return;
    if (!st->drag_active) return;

    int idx = st->drag_view_idx;
    if (idx >= 0 && idx < WM_MAX_VIEWS) {
        wm_view_t* v = &st->views[idx];
        if (v->mapped && v->surface_id != 0) {
            if (st->drag_is_resize) {
                if (st->drag_resize_new_w > 0 && st->drag_resize_new_h > 0) {
                    (void)comp_wm_move(c, v->client_id, v->surface_id, st->drag_resize_new_x, st->drag_resize_new_y);
                    v->x = st->drag_resize_new_x;
                    v->y = st->drag_resize_new_y;
                    (void)comp_wm_resize(c, v->client_id, v->surface_id, (int32_t)st->drag_resize_new_w, (int32_t)st->drag_resize_new_h);
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

static void wm_start_drag(comp_conn_t* c, wm_state_t* st, int idx, int32_t px, int32_t py, uint32_t button_mask, int requires_super) {
    if (!c || !st) return;
    if (idx < 0 || idx >= WM_MAX_VIEWS) return;
    wm_view_t* v = &st->views[idx];
    if (v->ui) return;
    if (!wm_is_view_visible_on_active_ws(st, v)) return;

    if (!v->floating) {
        v->floating = 1;
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

static uint32_t wm_resize_edges_for_point(const wm_view_t* v, int32_t px, int32_t py) {
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

static void wm_start_resize(comp_conn_t* c, wm_state_t* st, int idx, int32_t px, int32_t py, uint32_t button_mask, uint32_t edges) {
    if (!c || !st) return;
    if (idx < 0 || idx >= WM_MAX_VIEWS) return;
    if (edges == 0) return;

    wm_view_t* v = &st->views[idx];
    if (v->ui) return;
    if (!wm_is_view_visible_on_active_ws(st, v)) return;

    if (!v->floating) {
        v->floating = 1;
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

static void wm_hide_view(comp_conn_t* c, wm_view_t* v) {
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

static void wm_show_view(comp_conn_t* c, wm_view_t* v) {
    if (!c || !v || !v->mapped) return;
    if (v->ui) return;
    if (!v->hidden) return;
    v->hidden = 0;
    v->x = v->last_x;
    v->y = v->last_y;
    (void)comp_wm_move(c, v->client_id, v->surface_id, v->x, v->y);
}

static void wm_apply_layout(comp_conn_t* c, wm_state_t* st) {
    if (!c || !st) return;

    if (!st->have_screen) {
        uint32_t sw = 0, sh = 0;
        if (wm_read_fb_info(&sw, &sh) == 0) {
            st->screen_w = sw;
            st->screen_h = sh;
            st->have_screen = 1;
        }
    }

    int tiled[WM_MAX_VIEWS];
    int ntiled = 0;
    for (int i = 0; i < WM_MAX_VIEWS; i++) {
        wm_view_t* v = &st->views[i];
        if (!v->mapped) continue;
        if (v->ui) continue;
        if (v->workspace != st->active_ws) {
            wm_hide_view(c, v);
            continue;
        }
        if (v->hidden) wm_show_view(c, v);
        if (v->floating) continue;
        tiled[ntiled++] = i;
    }

    if (ntiled == 0) {
        wm_ui_raise_and_place(c, st);
        return;
    }

    int master_idx = wm_pick_master_idx(st, tiled, ntiled);
    if (master_idx < 0) return;

    const int32_t bar_h = (st->ui.client_id != COMP_WM_CLIENT_NONE) ? (int32_t)st->ui.h : 0;
    int32_t mx = st->gap_outer;
    int32_t my = st->gap_outer + bar_h;
    wm_view_t* master = &st->views[master_idx];
    master->x = mx;
    master->y = my;
    (void)comp_wm_move(c, master->client_id, master->surface_id, master->x, master->y);

    int32_t stack_x = master->x + (int32_t)master->w + st->gap_inner;
    int32_t stack_y = st->gap_outer + bar_h;
    for (int k = 0; k < ntiled; k++) {
        int idx = tiled[k];
        if (idx == master_idx) continue;
        wm_view_t* v = &st->views[idx];
        v->x = stack_x;
        v->y = stack_y;
        (void)comp_wm_move(c, v->client_id, v->surface_id, v->x, v->y);
        stack_y += (int32_t)v->h + st->gap_inner;
    }

    wm_ui_raise_and_place(c, st);
}

static void wm_focus_next(comp_conn_t* c, wm_state_t* st, int dir) {
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

static void wm_switch_workspace(comp_conn_t* c, wm_state_t* st, uint32_t ws) {
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

static void wm_move_focused_to_ws(comp_conn_t* c, wm_state_t* st, uint32_t ws) {
    if (!c || !st) return;
    if (ws >= WM_MAX_WORKSPACES) return;
    int idx = st->focused_idx;
    if (idx < 0 || idx >= WM_MAX_VIEWS) return;

    wm_view_t* v = &st->views[idx];
    if (!wm_is_view_visible_on_active_ws(st, v)) return;

    uint32_t old_ws = v->workspace;
    if (wm_master_matches(st, old_ws, v)) {
        wm_master_clear_for_ws(st, old_ws);
    }

    v->workspace = ws;
    if (st->master_surface_id[ws] == 0 && !v->floating) {
        wm_master_set_for_ws(st, ws, v->client_id, v->surface_id);
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

static void wm_toggle_floating(comp_conn_t* c, wm_state_t* st) {
    if (!c || !st) return;
    int idx = st->focused_idx;
    if (idx < 0 || idx >= WM_MAX_VIEWS) return;

    wm_view_t* v = &st->views[idx];
    if (!wm_is_view_visible_on_active_ws(st, v)) return;
    v->floating = !v->floating;
    wm_apply_layout(c, st);
}

static void wm_move_focused_float(comp_conn_t* c, wm_state_t* st, int32_t dx, int32_t dy) {
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

static void wm_close_focused(comp_conn_t* c, wm_state_t* st) {
    if (!c || !st) return;
    int idx = st->focused_idx;
    if (idx < 0 || idx >= WM_MAX_VIEWS) return;
    wm_view_t* v = &st->views[idx];
    if (!wm_is_view_visible_on_active_ws(st, v)) return;
    (void)comp_wm_close(c, v->client_id, v->surface_id);
}

static void wm_drop_view(wm_state_t* st, int idx) {
    if (!st) return;
    if (idx < 0 || idx >= WM_MAX_VIEWS) return;
    memset(&st->views[idx], 0, sizeof(st->views[idx]));
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

    if (!existed) {
        char tmp[96];
        (void)snprintf(tmp, sizeof(tmp), "wm: map c=%u s=%u %ux%u at %d,%d\n",
                      (unsigned)ev->client_id, (unsigned)ev->surface_id,
                      (unsigned)ev->sw, (unsigned)ev->sh,
                      (int)ev->sx, (int)ev->sy);
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
    wm_drop_view(st, idx);
    if (was_focused) wm_clear_focus(st);

    if (was_master) {
        wm_master_clear_for_ws(st, ws);
        wm_reselect_master_for_ws(st, ws);
    }

    if (was_focused) {
        for (int i = 0; i < WM_MAX_VIEWS; i++) {
            if (wm_is_view_visible_on_active_ws(st, &st->views[i]) && !st->views[i].ui) {
                wm_focus_view_idx(c, st, i);
                break;
            }
        }
        wm_ui_draw_bar(st);
    }
}

static void wm_on_commit(comp_conn_t* c, wm_state_t* st, const comp_ipc_wm_event_t* ev) {
    if (!c || !st || !ev) return;
    if (ev->surface_id == 0) return;
    if (ev->flags & COMP_WM_EVENT_FLAG_BACKGROUND) return;

    wm_view_t* v = wm_get_or_create_view(st, ev->client_id, ev->surface_id);
    if (!v) return;
    const int idx = (int)(v - st->views);
    v->w = ev->sw;
    v->h = ev->sh;

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

    if (!st->drag_active && !v->floating) {
        wm_apply_layout(c, st);
    }
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
    const int left_released = ((cur & left_mask) == 0u) && ((prev & left_mask) != 0u);
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
                    nx = st->drag_resize_start_x + (int32_t)st->drag_resize_start_w - (int32_t)WM_RESIZE_MIN_W;
                }
                nw = (int32_t)WM_RESIZE_MIN_W;
            }
            if (nh < (int32_t)WM_RESIZE_MIN_H) {
                if (st->drag_resize_edges & WM_RESIZE_EDGE_TOP) {
                    ny = st->drag_resize_start_y + (int32_t)st->drag_resize_start_h - (int32_t)WM_RESIZE_MIN_H;
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

    if (right_pressed) {
        if (ev->flags & COMP_WM_EVENT_FLAG_BACKGROUND) return;
        if (ev->surface_id == 0) return;
        int idx = wm_find_view_idx(st, ev->client_id, ev->surface_id);
        if (idx < 0) return;
        wm_focus_view_idx(c, st, idx);
        wm_view_t* v = &st->views[idx];
        uint32_t edges = wm_resize_edges_for_point(v, ev->px, ev->py);
        if (edges) {
            wm_start_resize(c, st, idx, ev->px, ev->py, right_mask, edges);
        } else {
            wm_start_drag(c, st, idx, ev->px, ev->py, right_mask, 0);
        }
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

    if (middle_pressed) {
        if (ev->flags & COMP_WM_EVENT_FLAG_BACKGROUND) return;
        if (ev->surface_id == 0) return;
        int idx = wm_find_view_idx(st, ev->client_id, ev->surface_id);
        if (idx < 0) return;
        wm_focus_view_idx(c, st, idx);
        wm_start_drag(c, st, idx, ev->px, ev->py, middle_mask, 0);
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

    if (st->run_mode) {
        if (kc == 0x1Bu) {
            st->run_mode = 0;
            st->run_len = 0;
            st->run_buf[0] = '\0';
        } else if (kc == 0x08u) {
            if (st->run_len > 0) {
                st->run_len--;
                st->run_buf[st->run_len] = '\0';
            }
        } else if (kc == 0x0Au) {
            if (st->run_len > 0) {
                (void)wm_spawn_app_by_name(st->run_buf);
            }
            st->run_mode = 0;
            st->run_len = 0;
            st->run_buf[0] = '\0';
        } else if (kc >= 32u && kc <= 126u) {
            if (kc != ' ') {
                if (st->run_len < (int)sizeof(st->run_buf) - 1) {
                    st->run_buf[st->run_len++] = (char)kc;
                    st->run_buf[st->run_len] = '\0';
                }
            }
        }

        wm_ui_draw_bar(st);
        wm_ui_raise_and_place(c, st);
        return;
    }

    if (kc >= 0x90u && kc <= 0x94u) {
        wm_switch_workspace(c, st, (uint32_t)(kc - 0x90u));
        return;
    }

    if (kc >= 0xA0u && kc <= 0xA4u) {
        wm_move_focused_to_ws(c, st, (uint32_t)(kc - 0xA0u));
        return;
    }

    if (kc == 0xA8u) {
        wm_close_focused(c, st);
        return;
    }

    if (kc == 0xA9u) {
        wm_focus_next(c, st, +1);
        return;
    }
    if (kc == 0xAAu) {
        wm_focus_next(c, st, -1);
        return;
    }

    if (kc == 0xABu) {
        wm_toggle_floating(c, st);
        return;
    }

    if (kc == 0xACu) {
        int idx = st->focused_idx;
        if (idx >= 0 && idx < WM_MAX_VIEWS) {
            wm_view_t* v = &st->views[idx];
            if (wm_is_view_visible_on_active_ws(st, v) && !v->floating) {
                wm_master_set_for_ws(st, st->active_ws, v->client_id, v->surface_id);
                wm_apply_layout(c, st);
            }
        }
        return;
    }

    if (kc == 0xB1u) {
        wm_move_focused_float(c, st, -st->float_step, 0);
        return;
    }
    if (kc == 0xB2u) {
        wm_move_focused_float(c, st, st->float_step, 0);
        return;
    }
    if (kc == 0xB3u) {
        wm_move_focused_float(c, st, 0, -st->float_step);
        return;
    }
    if (kc == 0xB4u) {
        wm_move_focused_float(c, st, 0, st->float_step);
        return;
    }
}

static int wm_handle_event(comp_conn_t* c, wm_state_t* st, const comp_ipc_wm_event_t* ev) {
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

__attribute__((force_align_arg_pointer)) int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    signal(2, (void*)on_signal);
    signal(15, (void*)on_signal);

    comp_conn_t c;
    comp_conn_reset(&c);

    wm_state_t st;
    memset(&st, 0, sizeof(st));
    st.active_ws = 0;
    st.focused_idx = -1;
    st.screen_w = 0;
    st.screen_h = 0;
    st.have_screen = 0;
    st.gap_outer = 10;
    st.gap_inner = 10;
    st.float_step = 20;
    st.super_down = 0;
    st.pointer_buttons = 0;
    st.pointer_x = 0;
    st.pointer_y = 0;
    st.drag_active = 0;
    st.drag_view_idx = -1;
    st.drag_off_x = 0;
    st.drag_off_y = 0;
    st.drag_button_mask = 0;
    st.drag_requires_super = 0;

    st.ui.client_id = COMP_WM_CLIENT_NONE;
    st.ui.surface_id = WM_UI_BAR_SURFACE_ID;
    st.ui.shm_fd = -1;

    while (!g_should_exit) {
        if (!c.connected) {
            if (comp_wm_connect(&c) == 0) {
                dbg_write("wm: connected\n");
                wm_reset_session_state(&st);
            } else {
                usleep(100000);
                continue;
            }
        }

        if (!st.ui.connected) {
            if (wm_ui_init(&st) != 0) {
                usleep(100000);
            }
        }

        if (st.ui.connected) {
            wm_ui_pump(&st.ui);
        }

        comp_ipc_hdr_t hdr;
        uint8_t payload[COMP_IPC_MAX_PAYLOAD];
        int r = comp_try_recv(&c, &hdr, payload, (uint32_t)sizeof(payload));
        if (r < 0) {
            dbg_write("wm: disconnected\n");
            comp_disconnect(&c);
            wm_reset_session_state(&st);
            usleep(100000);
            continue;
        }
        if (r == 0) {
            if (st.ui.connected) {
                wm_ui_pump(&st.ui);
            }
            usleep(1000);
            continue;
        }

        if (hdr.type == (uint16_t)COMP_IPC_MSG_WM_EVENT && hdr.len == (uint32_t)sizeof(comp_ipc_wm_event_t)) {
            comp_ipc_wm_event_t ev;
            memcpy(&ev, payload, sizeof(ev));
            (void)wm_handle_event(&c, &st, &ev);
        }
    }

    wm_ui_cleanup(&st.ui);
    comp_disconnect(&c);
    return 0;
}
