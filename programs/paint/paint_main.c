// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#include "paint_state.h"
#include "paint_util.h"
#include "paint_canvas.h"
#include "paint_image.h"
#include "paint_ui.h"
#include "paint_input.h"

#define SIGINT  2
#define SIGILL  4
#define SIGSEGV 11
#define SIGTERM 15

static void paint_on_signal(int sig) {
    char tmp[160];
    (void)snprintf(
        tmp,
        sizeof(tmp),
        "paint: signal=%d stage=%d win=%dx%d resize=%dx%d\n",
        sig,
        (int)g_dbg_stage,
        WIN_W,
        WIN_H,
        (int)g_dbg_resize_w,
        (int)g_dbg_resize_h
    );
    dbg_write(tmp);
    syscall(0, 128 + (uint32_t)sig, 0, 0);
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    dbg_write("paint: start\n");

    set_term_mode(0);

    g_dbg_stage = 1;

    (void)signal(SIGSEGV, (void*)paint_on_signal);
    (void)signal(SIGILL, (void*)paint_on_signal);
    (void)signal(SIGTERM, (void*)paint_on_signal);
    (void)signal(SIGINT, (void*)paint_on_signal);

    snapshot_init(&undo_stack[0], 'u');
    snapshot_init(&redo_stack[0], 'r');

    const uint32_t surface_id = 1u;
    uint32_t size_bytes = (uint32_t)WIN_W * (uint32_t)WIN_H * 4u;

    comp_conn_t conn;
    comp_conn_reset(&conn);
    if (comp_connect(&conn, "flux") != 0) {
        dbg_write("paint: comp_connect failed\n");
        return 1;
    }
    if (comp_send_hello(&conn) != 0) {
        dbg_write("paint: hello failed\n");
        comp_disconnect(&conn);
        return 1;
    }

    char shm_name[32];
    shm_name[0] = '\0';

    const int pid = getpid();
    int shm_fd = -1;
    int shm_gen = 0;
    for (int i = 0; i < 8; i++) {
        (void)snprintf(shm_name, sizeof(shm_name), "paint_%d_%d", pid, i);
        shm_fd = shm_create_named(shm_name, size_bytes);
        if (shm_fd >= 0) {
            break;
        }
    }
    if (shm_fd < 0) {
        dbg_write("paint: shm_create_named failed\n");
        comp_disconnect(&conn);
        return 1;
    }

    canvas = (uint32_t*)mmap(shm_fd, size_bytes, MAP_SHARED);
    if (ptr_is_invalid(canvas)) {
        dbg_write("paint: mmap(shm) failed\n");
        close(shm_fd);
        shm_unlink_named(shm_name);
        comp_disconnect(&conn);
        return 1;
    }

    layout_update();
    img_resize_to_canvas();

    render_all();

    if (comp_send_attach_shm_name(&conn, surface_id, shm_name, size_bytes, (uint32_t)WIN_W, (uint32_t)WIN_H, (uint32_t)WIN_W, 0u) != 0) {
        dbg_write("paint: attach_shm_name failed\n");
        munmap((void*)canvas, size_bytes);
        canvas = 0;
        close(shm_fd);
        shm_unlink_named(shm_name);
        comp_disconnect(&conn);
        return 1;
    }
    if (comp_send_commit(&conn, surface_id, 32, 32, 0u) != 0) {
        dbg_write("paint: commit failed\n");
        (void)comp_send_destroy_surface(&conn, surface_id, 0u);
        munmap((void*)canvas, size_bytes);
        canvas = 0;
        close(shm_fd);
        shm_unlink_named(shm_name);
        comp_disconnect(&conn);
        return 1;
    }

    dbg_write("paint: committed\n");

    int running = 1;
    int last_buttons = 0;
    int last_mx = 0;
    int last_my = 0;
    int have_mouse = 0;

    comp_ipc_hdr_t hdr;
    uint8_t payload[COMP_IPC_MAX_PAYLOAD];

    while (running) {
        int need_update = 0;

        for (;;) {
            int rr = comp_try_recv(&conn, &hdr, payload, (uint32_t)sizeof(payload));
            if (rr < 0) {
                running = 0;
                break;
            }
            if (rr == 0) {
                break;
            }

            if (hdr.type != (uint16_t)COMP_IPC_MSG_INPUT || hdr.len != (uint32_t)sizeof(comp_ipc_input_t)) {
                continue;
            }

            comp_ipc_input_t in;
            memcpy(&in, payload, sizeof(in));
            if (in.surface_id != surface_id) {
                continue;
            }

            if (in.kind == COMP_IPC_INPUT_RESIZE) {
                g_dbg_stage = 100;
                int32_t nw_i = in.x;
                int32_t nh_i = in.y;
                g_dbg_resize_w = nw_i;
                g_dbg_resize_h = nh_i;
                if (nw_i <= 0 || nh_i <= 0) {
                    continue;
                }
                if (nw_i == WIN_W && nh_i == WIN_H) {
                    continue;
                }

                const uint32_t max_pix = (uint32_t)PAINT_MAX_SURFACE_PIXELS;
                if (max_pix == 0) {
                    continue;
                }
                {
                    uint64_t want_pix64 = (uint64_t)(uint32_t)nw_i * (uint64_t)(uint32_t)nh_i;
                    if (want_pix64 > (uint64_t)max_pix) {
                        if (nw_i >= nh_i) {
                            nw_i = (int32_t)((uint32_t)max_pix / (uint32_t)nh_i);
                        } else {
                            nh_i = (int32_t)((uint32_t)max_pix / (uint32_t)nw_i);
                        }
                        if (nw_i <= 0 || nh_i <= 0) {
                            continue;
                        }
                    }
                }

                uint64_t bytes64 = (uint64_t)(uint32_t)nw_i * (uint64_t)(uint32_t)nh_i * 4u;
                if (bytes64 == 0 || bytes64 > 0xFFFFFFFFu || bytes64 > (uint64_t)PAINT_MAX_SURFACE_BYTES) {
                    continue;
                }
                uint32_t need_size_bytes = (uint32_t)bytes64;

                const int can_reuse_shm = (need_size_bytes <= size_bytes) && (shm_name[0] != '\0') && (shm_fd >= 0) && canvas;
                if (can_reuse_shm) {
                    g_dbg_stage = 110;
                    const int old_w = WIN_W;
                    const int old_h = WIN_H;
                    uint16_t err_code = 0;
                    if (comp_send_attach_shm_name_sync(&conn, surface_id, shm_name, size_bytes, (uint32_t)nw_i, (uint32_t)nh_i, (uint32_t)nw_i, 0u, 2000u, &err_code) != 0) {
                        char tmp[96];
                        (void)snprintf(tmp, sizeof(tmp), "paint: resize attach failed err=%u\n", (unsigned)err_code);
                        dbg_write(tmp);
                        continue;
                    }

                    g_dbg_stage = 120;
                    WIN_W = nw_i;
                    WIN_H = nh_i;
                    layout_update();
                    g_dbg_stage = 121;
                    img_resize_to_canvas();
                    g_dbg_stage = 122;
                    render_all();

                    g_dbg_stage = 130;
                    if (comp_send_commit_sync(&conn, surface_id, 32, 32, 0u, 2000u, &err_code) != 0) {
                        char tmp[96];
                        (void)snprintf(tmp, sizeof(tmp), "paint: resize commit failed err=%u\n", (unsigned)err_code);
                        dbg_write(tmp);

                        WIN_W = old_w;
                        WIN_H = old_h;
                        layout_update();
                        img_resize_to_canvas();
                        render_all();
                        continue;
                    }

                    mouse_down = 0;
                    drag_active = 0;
                    have_mouse = 0;
                    last_buttons = 0;
                    need_update = 0;
                    continue;
                }

                uint64_t grow64 = (uint64_t)size_bytes * 2ull;
                uint64_t new_cap64 = (grow64 >= (uint64_t)need_size_bytes) ? grow64 : (uint64_t)need_size_bytes;
                if (new_cap64 > (uint64_t)PAINT_MAX_SURFACE_BYTES) {
                    new_cap64 = (uint64_t)need_size_bytes;
                }
                if (new_cap64 > 0xFFFFFFFFu) {
                    continue;
                }
                uint32_t new_cap_bytes = (uint32_t)new_cap64;

                char new_shm_name[32];
                new_shm_name[0] = '\0';
                int new_shm_fd = -1;
                g_dbg_stage = 200;
                for (int i = 0; i < 16; i++) {
                    shm_gen++;
                    (void)snprintf(new_shm_name, sizeof(new_shm_name), "paint_%d_r%d", pid, shm_gen);
                    new_shm_fd = shm_create_named(new_shm_name, new_cap_bytes);
                    if (new_shm_fd >= 0) {
                        break;
                    }
                }
                if (new_shm_fd < 0) {
                    continue;
                }

                g_dbg_stage = 210;
                uint32_t* new_canvas = (uint32_t*)mmap(new_shm_fd, new_cap_bytes, MAP_SHARED);
                if (ptr_is_invalid(new_canvas)) {
                    dbg_write("paint: resize mmap failed\n");
                    close(new_shm_fd);
                    shm_unlink_named(new_shm_name);
                    continue;
                }

                uint16_t err_code = 0;
                g_dbg_stage = 220;
                if (comp_send_attach_shm_name_sync(
                        &conn,
                        surface_id,
                        new_shm_name,
                        new_cap_bytes,
                        (uint32_t)nw_i,
                        (uint32_t)nh_i,
                        (uint32_t)nw_i,
                        0u,
                        2000u,
                        &err_code
                    ) != 0) {
                    char tmp[96];
                    (void)snprintf(tmp, sizeof(tmp), "paint: resize attach(new) failed err=%u\n", (unsigned)err_code);
                    dbg_write(tmp);
                    munmap((void*)new_canvas, new_cap_bytes);
                    close(new_shm_fd);
                    shm_unlink_named(new_shm_name);
                    continue;
                }

                uint32_t* old_canvas = canvas;
                uint32_t old_size_bytes = size_bytes;
                int old_shm_fd = shm_fd;
                int old_w = WIN_W;
                int old_h = WIN_H;
                char old_shm_name[32];
                memcpy(old_shm_name, shm_name, sizeof(old_shm_name));

                canvas = new_canvas;
                WIN_W = nw_i;
                WIN_H = nh_i;
                size_bytes = new_cap_bytes;
                shm_fd = new_shm_fd;
                memcpy(shm_name, new_shm_name, sizeof(shm_name));

                g_dbg_stage = 230;
                layout_update();
                g_dbg_stage = 231;
                img_resize_to_canvas();
                g_dbg_stage = 232;
                render_all();

                g_dbg_stage = 240;
                if (comp_send_commit_sync(&conn, surface_id, 32, 32, 0u, 2000u, &err_code) != 0) {
                    char tmp[96];
                    (void)snprintf(tmp, sizeof(tmp), "paint: resize commit(new) failed err=%u\n", (unsigned)err_code);
                    dbg_write(tmp);

                    canvas = old_canvas;
                    WIN_W = old_w;
                    WIN_H = old_h;
                    size_bytes = old_size_bytes;
                    shm_fd = old_shm_fd;
                    memcpy(shm_name, old_shm_name, sizeof(shm_name));
                    layout_update();
                    img_resize_to_canvas();
                    render_all();

                    munmap((void*)new_canvas, new_cap_bytes);
                    close(new_shm_fd);
                    shm_unlink_named(new_shm_name);
                    need_update = 1;
                    continue;
                }

                if (old_canvas) {
                    munmap((void*)old_canvas, old_size_bytes);
                }
                if (old_shm_fd >= 0) {
                    close(old_shm_fd);
                }
                if (old_shm_name[0]) {
                    shm_unlink_named(old_shm_name);
                }

                mouse_down = 0;
                drag_active = 0;
                have_mouse = 0;
                last_buttons = 0;
                need_update = 0;
                continue;
            }

            if (in.kind == COMP_IPC_INPUT_KEY) {
                if (in.key_state == 1u) {
                    (void)handle_key((unsigned char)(uint8_t)in.keycode);
                    need_update = 1;
                }
                continue;
            }

            if (in.kind == COMP_IPC_INPUT_CLOSE) {
                running = 0;
                break;
            }

            if (in.kind == COMP_IPC_INPUT_MOUSE) {
                const int mx = (int)in.x;
                const int my = (int)in.y;
                const int buttons = (int)in.buttons;

                const int prev_buttons = have_mouse ? last_buttons : 0;
                if (!have_mouse) {
                    last_mx = mx;
                    last_my = my;
                    have_mouse = 1;
                }

                const int down_now = (buttons & 1) != 0;
                const int down_prev = (prev_buttons & 1) != 0;

                if (down_now && !down_prev) {
                    g_dbg_stage = 310;
                    handle_mouse_down(mx, my);
                    need_update = 1;
                }
                if (!down_now && down_prev) {
                    g_dbg_stage = 330;
                    handle_mouse_up();
                    need_update = 1;
                }
                if (mx != last_mx || my != last_my) {
                    if (mouse_down) {
                        g_dbg_stage = 320;
                        handle_mouse_move(mx, my);
                        need_update = 1;
                    }
                }

                last_mx = mx;
                last_my = my;
                last_buttons = buttons;
            }
        }

        if (need_update && canvas) {
            g_dbg_stage = 400;
            render_all();
            g_dbg_stage = 401;
            if (comp_send_commit(&conn, surface_id, 32, 32, 0u) != 0) {
                dbg_write("paint: commit failed\n");
                running = 0;
                break;
            }
        }
        comp_wait_events(&conn, 16000u);
    }

    for (int i = 0; i < undo_count; i++) {
        snapshot_free(&undo_stack[i]);
    }
    for (int i = 0; i < redo_count; i++) {
        snapshot_free(&redo_stack[i]);
    }
    if (img && !ptr_is_invalid(img) && img_cap_bytes) {
        munmap((void*)img, img_cap_bytes);
    }
    img = 0;
    if (img_shm_fd >= 0) {
        close(img_shm_fd);
        img_shm_fd = -1;
    }
    if (img_shm_name[0]) {
        (void)shm_unlink_named(img_shm_name);
        img_shm_name[0] = '\0';
    }
    img_cap_bytes = 0;

    (void)comp_send_destroy_surface(&conn, surface_id, 0u);
    comp_disconnect(&conn);

    if (canvas) {
        munmap((void*)canvas, size_bytes);
        canvas = 0;
    }
    if (shm_fd >= 0) {
        close(shm_fd);
        shm_fd = -1;
    }
    if (shm_name[0]) {
        shm_unlink_named(shm_name);
        shm_name[0] = '\0';
    }

    return 0;
}
