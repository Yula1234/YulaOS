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

    int pid = spawn_process_resolved(name, 1, argv);

    {
        char tmp[128];
        (void)snprintf(tmp, sizeof(tmp), "wm: spawn name='%s' pid=%d\n", name, pid);
        dbg_write(tmp);
    }
    return pid;
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
