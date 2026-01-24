#include "wm_internal.h"

void wm_ui_cleanup(wm_ui_t* ui) {
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

void wm_ui_pump(wm_ui_t* ui) {
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

void wm_ui_raise_and_place(comp_conn_t* wm_conn, wm_state_t* st) {
    if (!wm_conn || !st) return;
    if (st->ui.client_id == COMP_WM_CLIENT_NONE || st->ui.surface_id == 0) return;
    (void)comp_wm_move(wm_conn, st->ui.client_id, st->ui.surface_id, 0, 0);
    (void)comp_wm_raise(wm_conn, st->ui.client_id, st->ui.surface_id);
}

int wm_spawn_app_by_name(const char* name) {
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

    const char* labels[] = {"Paint", "Explorer", "GEditor"};
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

void wm_ui_handle_bar_click(comp_conn_t* c, wm_state_t* st, int32_t x) {
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

void wm_ui_draw_bar(wm_state_t* st) {
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
            const char* labels[] = {"Paint", "Explorer", "GEditor"};
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

    int r = comp_send_commit(&ui->c, ui->surface_id, 0, 0, 0u);
    if (r != 0) {
        dbg_write("wm_ui: draw commit send failed\n");
        wm_ui_cleanup(ui);
        return;
    }
    wm_ui_pump(ui);
}

int wm_ui_init(wm_state_t* st) {
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
    r = comp_send_attach_shm_name_sync(&ui->c, ui->surface_id, ui->shm_name, ui->size_bytes, ui->w, ui->h, ui->w,
                                      0u, 2000u, &err);
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
