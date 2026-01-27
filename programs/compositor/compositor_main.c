#include "compositor_internal.h"

static void on_signal(int sig) {
    (void)sig;
    if (!g_fb_released) {
        fb_release();
        g_fb_released = 1;
    }
    g_should_exit = 1;

    sigreturn();
    for (;;) {
    }
}

static void on_sigint_ignore(int sig) {
    (void)sig;
    sigreturn();
    for (;;) {
    }
}

typedef struct {
    int valid;
    int x;
    int y;
    int w;
    int h;
    int stride;
    uint32_t z;
    const uint32_t* pixels;
    uint32_t commit_gen;
} draw_surface_state_t;

typedef struct {
    uint32_t z;
    int ci;
    int si;
} draw_item_t;

static void comp_client_slot_reset(comp_client_t* c) {
    if (!c) return;
    memset(c, 0, sizeof(*c));
    c->connected = 0;
    c->pid = -1;
    c->fd_c2s = -1;
    c->fd_s2c = -1;
    ipc_rx_reset(&c->rx);
    c->input_ring_shm_fd = -1;
    c->input_ring_size_bytes = 0;
    c->input_ring_shm_name[0] = '\0';
    c->input_ring = 0;
    c->input_ring_enabled = 0;
    c->seq_out = 1;
    c->z_counter = 1;
    for (int i = 0; i < COMP_MAX_SURFACES; i++) {
        c->surfaces[i].shm_fd = -1;
        for (int bi = 0; bi < COMP_SURFACE_SHADOW_BUFS; bi++) {
            c->surfaces[i].shadow_shm_fd[bi] = -1;
        }
    }
}

static int comp_clients_reserve(comp_client_t** clients,
                                int* clients_cap,
                                int want_cap,
                                draw_surface_state_t** prev_state,
                                int* prev_state_cap_clients,
                                draw_item_t** order,
                                int* order_cap) {
    if (!clients || !clients_cap || !prev_state || !prev_state_cap_clients || !order || !order_cap) return -1;
    if (want_cap <= 0) want_cap = 1;
    if (*clients_cap >= want_cap) return 0;

    int old_cap = *clients_cap;
    int new_cap = old_cap > 0 ? old_cap : (int)COMP_CLIENTS_INIT;
    while (new_cap < want_cap) {
        if (new_cap > (1 << 20)) {
            new_cap = want_cap;
            break;
        }
        new_cap *= 2;
        if (new_cap <= 0) {
            new_cap = want_cap;
            break;
        }
    }

    size_t new_clients_bytes = (size_t)new_cap * sizeof(comp_client_t);
    size_t new_prev_n = (size_t)new_cap * (size_t)COMP_MAX_SURFACES;
    size_t new_prev_bytes = new_prev_n * sizeof(draw_surface_state_t);
    size_t new_order_n = (size_t)new_cap * (size_t)COMP_MAX_SURFACES;
    size_t new_order_bytes = new_order_n * sizeof(draw_item_t);

    comp_client_t* new_clients = (comp_client_t*)malloc(new_clients_bytes);
    draw_surface_state_t* new_prev = (draw_surface_state_t*)malloc(new_prev_bytes);
    draw_item_t* new_order = (draw_item_t*)malloc(new_order_bytes);

    if (!new_clients || !new_prev || !new_order) {
        if (new_clients) free(new_clients);
        if (new_prev) free(new_prev);
        if (new_order) free(new_order);
        return -1;
    }

    for (int i = 0; i < new_cap; i++) {
        comp_client_slot_reset(&new_clients[i]);
    }
    if (*clients && old_cap > 0) {
        memcpy(new_clients, *clients, (size_t)old_cap * sizeof(comp_client_t));
    }

    memset(new_prev, 0, new_prev_bytes);
    if (*prev_state && *prev_state_cap_clients > 0) {
        const int copy_clients = (*prev_state_cap_clients < new_cap) ? *prev_state_cap_clients : new_cap;
        const size_t copy_n = (size_t)copy_clients * (size_t)COMP_MAX_SURFACES;
        memcpy(new_prev, *prev_state, copy_n * sizeof(draw_surface_state_t));
    }

    if (*clients) free(*clients);
    if (*prev_state) free(*prev_state);
    if (*order) free(*order);

    *clients = new_clients;
    *clients_cap = new_cap;
    *prev_state = new_prev;
    *prev_state_cap_clients = new_cap;
    *order = new_order;
    *order_cap = (int)new_order_n;
    return 0;
}

__attribute__((force_align_arg_pointer)) int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    dbg_write("compositor: enter main\n");

    dbg_write("compositor: install signals\n");
    signal(2, (void*)on_sigint_ignore);
    signal(15, (void*)on_signal);
    dbg_write("compositor: signals ok\n");

    dbg_write("compositor: open /dev/fb0\n");
    int fd_fb = open("/dev/fb0", 0);
    if (fd_fb < 0) {
        dbg_write("compositor: cannot open /dev/fb0\n");
        return 1;
    }

    dbg_write("compositor: read fb info\n");
    fb_info_t info;
    int r = read(fd_fb, &info, sizeof(info));
    close(fd_fb);
    dbg_write("compositor: fb info read done\n");

    if (r < (int)sizeof(info) || info.width == 0 || info.height == 0 || info.pitch == 0) {
        dbg_write("compositor: bad fb info\n");
        return 1;
    }

    dbg_write("compositor: open /dev/mouse\n");
    int fd_mouse = open("/dev/mouse", 0);
    if (fd_mouse < 0) {
        dbg_write("compositor: open mouse failed\n");
        return 1;
    }

    int listen_fd = -1;
    int wm_listen_fd = -1;

    dbg_write("compositor: fb_acquire\n");
    if (fb_acquire() != 0) {
        dbg_write("compositor: fb busy\n");
        close(fd_mouse);
        return 1;
    }
    dbg_write("compositor: fb acquired\n");

    dbg_write("compositor: map_framebuffer\n");
    uint32_t* fb = (uint32_t*)map_framebuffer();
    if (!fb) {
        close(fd_mouse);
        fb_release();
        g_fb_released = 1;
        dbg_write("compositor: map_framebuffer failed\n");
        return 1;
    }
    dbg_write("compositor: fb mapped\n");

    int w = (int)info.width;
    int h = (int)info.height;
    int stride = (int)(info.pitch / 4u);
    if (stride <= 0) stride = w;

    g_screen_w = w;
    g_screen_h = h;

    int frame_shm_fd = -1;
    uint32_t* frame_pixels = 0;
    uint32_t frame_size_bytes = 0;
    {
        uint64_t fb_bytes64 = (uint64_t)info.pitch * (uint64_t)info.height;
        if (fb_bytes64 > 0 && fb_bytes64 <= 0xFFFFFFFFu) {
            frame_size_bytes = (uint32_t)fb_bytes64;
            frame_shm_fd = shm_create(frame_size_bytes);
            if (frame_shm_fd >= 0) {
                frame_pixels = (uint32_t*)mmap(frame_shm_fd, frame_size_bytes, MAP_SHARED);
                if (!frame_pixels) {
                    close(frame_shm_fd);
                    frame_shm_fd = -1;
                }
            }
        }
    }

    int shm_w = 320;
    int shm_h = 240;
    uint32_t shm_size = (uint32_t)shm_w * (uint32_t)shm_h * 4u;
    int shm_fd = shm_create(shm_size);
    if (shm_fd < 0) {
        dbg_write("compositor: shm_create failed\n");
    }

    comp_buffer_t buf;
    memset(&buf, 0, sizeof(buf));
    buf.shm_fd = shm_fd;
    buf.size_bytes = shm_size;
    buf.w = shm_w;
    buf.h = shm_h;
    buf.stride = shm_w;
    buf.pixels = 0;
    if (buf.shm_fd >= 0) {
        buf.pixels = (uint32_t*)mmap(buf.shm_fd, buf.size_bytes, MAP_SHARED);
        if (!buf.pixels) {
            dbg_write("compositor: mmap(shm) failed\n");
        }
    }

    int ipc_fds[2] = { -1, -1 };
    int ipc_back[2] = { -1, -1 };
    int have_ipc = 0;
    int child_pid = -1;
    if (buf.shm_fd >= 0 && buf.pixels && pipe(ipc_fds) == 0 && pipe(ipc_back) == 0) {
        char shm_s[16];
        char w_s[16];
        char h_s[16];
        char c2s_w_s[16];
        char s2c_r_s[16];
        char c2s_r_s[16];
        char s2c_w_s[16];

        itoa(buf.shm_fd, shm_s, 10);
        itoa(shm_w, w_s, 10);
        itoa(shm_h, h_s, 10);
        itoa(ipc_fds[1], c2s_w_s, 10);
        itoa(ipc_back[0], s2c_r_s, 10);
        itoa(ipc_fds[0], c2s_r_s, 10);
        itoa(ipc_back[1], s2c_w_s, 10);

        char* argv2[8];
        argv2[0] = (char*)"comp_client";
        argv2[1] = shm_s;
        argv2[2] = w_s;
        argv2[3] = h_s;
        argv2[4] = c2s_w_s;
        argv2[5] = s2c_r_s;
        argv2[6] = c2s_r_s;
        argv2[7] = s2c_w_s;

        child_pid = spawn_process("/bin/comp_client.exe", 8, argv2);
        if (child_pid >= 0) {
            have_ipc = 1;
            close(ipc_fds[1]);
            close(ipc_back[0]);
        } else {
            dbg_write("compositor: spawn comp_client failed\n");
            close(ipc_fds[0]);
            close(ipc_fds[1]);
            close(ipc_back[0]);
            close(ipc_back[1]);
            ipc_fds[0] = -1;
            ipc_fds[1] = -1;
            ipc_back[0] = -1;
            ipc_back[1] = -1;
        }
    } else {
        if (ipc_fds[0] >= 0) close(ipc_fds[0]);
        if (ipc_fds[1] >= 0) close(ipc_fds[1]);
        if (ipc_back[0] >= 0) close(ipc_back[0]);
        if (ipc_back[1] >= 0) close(ipc_back[1]);
        ipc_fds[0] = -1;
        ipc_fds[1] = -1;
        ipc_back[0] = -1;
        ipc_back[1] = -1;
    }

    comp_client_t* clients = 0;
    int clients_cap = 0;
    draw_surface_state_t* prev_state = 0;
    int prev_state_cap_clients = 0;
    draw_item_t* order = 0;
    int order_cap = 0;

    if (comp_clients_reserve(&clients, &clients_cap, (int)COMP_CLIENTS_INIT, &prev_state, &prev_state_cap_clients, &order, &order_cap) != 0) {
        dbg_write("compositor: OOM: cannot allocate clients\n");
        close(fd_mouse);
        fb_release();
        g_fb_released = 1;
        return 1;
    }

    if (have_ipc) {
        comp_client_init(&clients[0], child_pid, ipc_fds[0], ipc_back[1]);
    }

    comp_input_state_t input;
    comp_input_state_init(&input);

    uint32_t z_counter = 1;

    listen_fd = ipc_listen("compositor");
    if (listen_fd < 0) {
        dbg_write("compositor: ipc_listen failed\n");
    }

    wm_conn_t wm;
    memset(&wm, 0, sizeof(wm));
    wm.fd_c2s = -1;
    wm.fd_s2c = -1;
    wm.connected = 0;
    ipc_rx_reset(&wm.rx);
    wm.seq_out = 1;

    wm_listen_fd = ipc_listen("compositor_wm");
    if (wm_listen_fd < 0) {
        dbg_write("compositor: ipc_listen compositor_wm failed\n");
    }

    int wm_pid = -1;
    int wm_spawn_cooldown = 0;
    int wm_spawn_retry_wait = 0;

    mouse_state_t ms_last;
    ms_last.x = w / 2;
    ms_last.y = h / 2;
    ms_last.buttons = 0;

    int32_t draw_mx = 0x7FFFFFFF;
    int32_t draw_my = 0x7FFFFFFF;

    comp_rect_t prev_preview_rect;
    prev_preview_rect.x1 = 0;
    prev_preview_rect.y1 = 0;
    prev_preview_rect.x2 = 0;
    prev_preview_rect.y2 = 0;

    comp_preview_t preview;
    memset(&preview, 0, sizeof(preview));
    int preview_dirty = 0;

    int first_frame = 1;

    while (!g_should_exit) {
        int scene_dirty = 0;
        if (wm_spawn_retry_wait > 0) wm_spawn_retry_wait--;
        if (!wm.connected && wm_pid > 0) {
            if (wm_spawn_cooldown > 0) {
                wm_spawn_cooldown--;
            } else {
                wm_pid = -1;
            }
        }

        if (wm_listen_fd < 0) {
            wm_listen_fd = ipc_listen("compositor_wm");
        }
        if (!wm.connected && wm_listen_fd >= 0) {
            int fds[2] = { -1, -1 };
            int ar = ipc_accept(wm_listen_fd, fds);
            if (ar == 1) {
                wm_init(&wm, fds[0], fds[1]);
                if (wm_pid < 0) {
                    wm_pid = 0;
                }
                wm_replay_state(&wm, clients, clients_cap);
            }
        }

        if (!wm.connected && wm_pid < 0 && wm_spawn_retry_wait == 0 && listen_fd >= 0 && wm_listen_fd >= 0) {
            char* wargv[1];
            wargv[0] = (char*)"wm";
            wm_pid = spawn_process("/bin/wm.exe", 1, wargv);
            if (wm_pid < 0) {
                dbg_write("compositor: spawn wm failed\n");
                wm_spawn_retry_wait = 200;
            } else {
                wm_spawn_cooldown = 200;
            }
        }

        if (wm.connected) {
            wm_pump(&wm, clients, clients_cap, &input, &z_counter, &preview, &preview_dirty, &scene_dirty);
            if (!wm.connected) {
                input.focus_client = -1;
                input.focus_surface_id = 0;
                input.wm_pointer_grab_active = 0;
                input.wm_pointer_grab_client = -1;
                input.wm_pointer_grab_surface_id = 0;
                if (preview.active) {
                    preview.active = 0;
                    preview_dirty = 1;
                }
            }
        }

        if (listen_fd >= 0) {
            for (;;) {
                int fds[2] = { -1, -1 };
                int ar = ipc_accept(listen_fd, fds);
                if (ar != 1) break;

                int slot = -1;
                for (int i = 0; i < clients_cap; i++) {
                    if (!clients[i].connected) {
                        slot = i;
                        break;
                    }
                }

                if (slot < 0) {
                    const int want = clients_cap + 1;
                    if (comp_clients_reserve(&clients, &clients_cap, want, &prev_state, &prev_state_cap_clients, &order, &order_cap) == 0) {
                        slot = want - 1;
                    }
                }

                if (slot >= 0 && slot < clients_cap) {
                    comp_client_init(&clients[slot], -1, fds[0], fds[1]);
                    dbg_write("compositor: accepted client\n");
                } else {
                    dbg_write("compositor: reject client (OOM)\n");
                    if (fds[0] >= 0) close(fds[0]);
                    if (fds[1] >= 0) close(fds[1]);
                }
            }
        }

        for (int ci = 0; ci < clients_cap; ci++) {
            if (!clients[ci].connected) continue;
            comp_client_pump(&clients[ci], &buf, &z_counter, &wm, (uint32_t)ci);
        }

        if (wm.connected) {
            wm_pump(&wm, clients, clients_cap, &input, &z_counter, &preview, &preview_dirty, &scene_dirty);
        }

        mouse_state_t ms;
        int mr = read(fd_mouse, &ms, sizeof(ms));
        if (mr < (int)sizeof(ms)) {
            ms = ms_last;
        } else {
            ms_last = ms;
        }

        comp_update_focus(clients, clients_cap, &input, &ms, &z_counter, &wm);

        if (wm.connected) {
            comp_send_wm_pointer(&wm, clients, clients_cap, &input, &ms);
            if (wm.connected) {
                wm_pump(&wm, clients, clients_cap, &input, &z_counter, &preview, &preview_dirty, &scene_dirty);
            }
            if (!wm.connected) {
                input.focus_client = -1;
                input.focus_surface_id = 0;
                input.wm_pointer_grab_active = 0;
                input.wm_pointer_grab_client = -1;
                input.wm_pointer_grab_surface_id = 0;
                if (preview.active) {
                    preview.active = 0;
                    preview_dirty = 1;
                }
            }
        }

        if (comp_send_mouse(clients, clients_cap, &input, &ms) < 0) {
            int dc = input.last_client;
            if (dc >= 0 && dc < clients_cap && clients[dc].connected) {
                dbg_write("compositor: client disconnected\n");
                if (wm.connected) {
                    for (int si = 0; si < COMP_MAX_SURFACES; si++) {
                        comp_surface_t* s = &clients[dc].surfaces[si];
                        if (!s->in_use) continue;
                        comp_ipc_wm_event_t ev;
                        memset(&ev, 0, sizeof(ev));
                        ev.kind = COMP_WM_EVENT_UNMAP;
                        ev.client_id = (uint32_t)dc;
                        ev.surface_id = s->id;
                        ev.flags = 0;
                        if (wm_send_event(&wm, &ev, 1) < 0) {
                            wm_disconnect(&wm);
                            input.focus_client = -1;
                            input.focus_surface_id = 0;
                            break;
                        }
                    }
                }
                comp_client_disconnect(&clients[dc]);
            }
        }

        for (;;) {
            char kc = 0;
            int kr = kbd_try_read(&kc);
            if (kr <= 0) break;

            if (wm.connected) {
                comp_ipc_wm_event_t ev;
                memset(&ev, 0, sizeof(ev));
                ev.kind = COMP_WM_EVENT_KEY;
                ev.client_id = input.focus_client >= 0 ? (uint32_t)input.focus_client : COMP_WM_CLIENT_NONE;
                ev.surface_id = input.focus_surface_id;
                ev.keycode = (uint32_t)(uint8_t)kc;
                ev.key_state = 1u;

                if (input.focus_client >= 0 && input.focus_client < clients_cap) {
                    comp_client_t* c = &clients[input.focus_client];
                    comp_surface_t* s = comp_client_surface_find(c, input.focus_surface_id);
                    if (s && s->attached && s->committed) {
                        ev.sx = (int32_t)s->x;
                        ev.sy = (int32_t)s->y;
                        ev.sw = (uint32_t)s->w;
                        ev.sh = (uint32_t)s->h;
                    }
                }

                if (wm_send_event(&wm, &ev, 1) < 0) {
                    wm_disconnect(&wm);
                    input.focus_client = -1;
                    input.focus_surface_id = 0;
                }
            }

            if (comp_send_key(clients, clients_cap, &input, (uint32_t)(uint8_t)kc, 1u) < 0) {
                int dc = input.focus_client;
                if (dc >= 0 && dc < clients_cap && clients[dc].connected) {
                    dbg_write("compositor: client disconnected\n");
                    if (wm.connected) {
                        for (int si = 0; si < COMP_MAX_SURFACES; si++) {
                            comp_surface_t* s = &clients[dc].surfaces[si];
                            if (!s->in_use) continue;
                            comp_ipc_wm_event_t ev;
                            memset(&ev, 0, sizeof(ev));
                            ev.kind = COMP_WM_EVENT_UNMAP;
                            ev.client_id = (uint32_t)dc;
                            ev.surface_id = s->id;
                            ev.flags = 0;
                            if (wm_send_event(&wm, &ev, 1) < 0) {
                                wm_disconnect(&wm);
                                input.focus_client = -1;
                                input.focus_surface_id = 0;
                                break;
                            }
                        }
                    }
                    comp_client_disconnect(&clients[dc]);
                }
                break;
            }
        }

        if (wm.connected) {
            wm_flush_tx(&wm);
            if (!wm.connected) {
                input.focus_client = -1;
                input.focus_surface_id = 0;
                input.wm_pointer_grab_active = 0;
                input.wm_pointer_grab_client = -1;
                input.wm_pointer_grab_surface_id = 0;
                if (preview.active) {
                    preview.active = 0;
                    preview_dirty = 1;
                }
            }
        }

        comp_damage_t dmg;
        damage_reset(&dmg);
        int any_surface_changed = 0;

        if (scene_dirty) {
            damage_add(&dmg, rect_make(0, 0, w, h), w, h);
        }

        if (first_frame) {
            damage_add(&dmg, rect_make(0, 0, w, h), w, h);
        }

        comp_rect_t new_preview_rect;
        new_preview_rect.x1 = 0;
        new_preview_rect.y1 = 0;
        new_preview_rect.x2 = 0;
        new_preview_rect.y2 = 0;
        if (preview.active && preview.client_id < (uint32_t)clients_cap && preview.w > 0 && preview.h > 0) {
            comp_client_t* pc = &clients[(int)preview.client_id];
            if (pc->connected) {
                comp_surface_t* ps = comp_client_surface_find(pc, preview.surface_id);
                if (ps && ps->in_use && ps->attached && ps->committed) {
                    const int t = 2;
                    new_preview_rect = rect_make(ps->x - t, ps->y - t, (int)preview.w + t * 2, (int)preview.h + t * 2);
                    new_preview_rect = rect_clip_to_screen(new_preview_rect, w, h);
                }
            }
        }
        if (preview_dirty || !rect_empty(&prev_preview_rect) || !rect_empty(&new_preview_rect)) {
            if (preview_dirty || prev_preview_rect.x1 != new_preview_rect.x1 || prev_preview_rect.y1 != new_preview_rect.y1 || prev_preview_rect.x2 != new_preview_rect.x2 || prev_preview_rect.y2 != new_preview_rect.y2) {
                if (!rect_empty(&prev_preview_rect)) damage_add(&dmg, prev_preview_rect, w, h);
                if (!rect_empty(&new_preview_rect)) damage_add(&dmg, new_preview_rect, w, h);
            }
        }

        for (int ci = 0; ci < clients_cap; ci++) {
            for (int si = 0; si < COMP_MAX_SURFACES; si++) {
                const int idx = ci * COMP_MAX_SURFACES + si;
                const comp_surface_t* s = &clients[ci].surfaces[si];
                const int curr_valid = (clients[ci].connected && s->in_use && s->attached && s->committed && s->pixels && s->w > 0 && s->h > 0 && s->stride > 0);

                draw_surface_state_t cur;
                memset(&cur, 0, sizeof(cur));
                cur.valid = curr_valid;
                if (curr_valid) {
                    cur.x = s->x;
                    cur.y = s->y;
                    cur.w = s->w;
                    cur.h = s->h;
                    cur.stride = s->stride;
                    cur.z = s->z;
                    cur.pixels = s->pixels;
                    cur.commit_gen = s->commit_gen;
                }

                const draw_surface_state_t* prev = &prev_state[idx];
                int changed = 0;
                if (prev->valid != cur.valid) {
                    changed = 1;
                } else if (cur.valid) {
                    if (prev->x != cur.x || prev->y != cur.y || prev->w != cur.w || prev->h != cur.h) changed = 1;
                    else if (prev->stride != cur.stride) changed = 1;
                    else if (prev->z != cur.z) changed = 1;
                    else if (prev->pixels != cur.pixels) changed = 1;
                    else if (prev->commit_gen != cur.commit_gen) changed = 1;
                }

                if (changed) {
                    any_surface_changed = 1;
                    if (prev->valid) damage_add(&dmg, rect_make(prev->x, prev->y, prev->w, prev->h), w, h);
                    if (cur.valid) damage_add(&dmg, rect_make(cur.x, cur.y, cur.w, cur.h), w, h);
                }

                prev_state[idx] = cur;
            }
        }

        const int cursor_moved = (ms.x != draw_mx || ms.y != draw_my);
        if (cursor_moved || dmg.n > 0) {
            comp_cursor_restore(fb, stride, w, h);
        }

        if (dmg.n > 0) {
            preview_dirty = 0;
            prev_preview_rect = new_preview_rect;

            uint32_t bg = 0x101010;
            uint32_t* out = frame_pixels ? frame_pixels : fb;

            int order_n = 0;
            for (int ci = 0; ci < clients_cap; ci++) {
                if (!clients[ci].connected) continue;
                for (int si = 0; si < COMP_MAX_SURFACES; si++) {
                    comp_surface_t* s = &clients[ci].surfaces[si];
                    if (!s->in_use || !s->attached || !s->committed) continue;
                    const uint32_t* src = 0;
                    int src_stride = 0;
                    if (s->shadow_valid && s->shadow_active >= 0 && s->shadow_active < COMP_SURFACE_SHADOW_BUFS) {
                        src = s->shadow_pixels[s->shadow_active];
                        src_stride = s->shadow_stride;
                    }
                    if (!src) {
                        src = s->pixels;
                        src_stride = s->stride;
                    }
                    if (!src || s->w <= 0 || s->h <= 0 || src_stride <= 0) continue;
                    order[order_n].z = s->z;
                    order[order_n].ci = ci;
                    order[order_n].si = si;
                    order_n++;
                }
            }

            for (int i = 1; i < order_n; i++) {
                draw_item_t key = order[i];
                int j = i - 1;
                while (j >= 0 && order[j].z > key.z) {
                    order[j + 1] = order[j];
                    j--;
                }
                order[j + 1] = key;
            }

            const uint32_t preview_col = 0x007ACC;

            for (int ri = 0; ri < dmg.n; ri++) {
                const comp_rect_t clip = dmg.rects[ri];
                if (rect_empty(&clip)) continue;

                if (!frame_pixels || first_frame || any_surface_changed || scene_dirty) {
                    fill_rect(out, stride, w, h, clip.x1, clip.y1, clip.x2 - clip.x1, clip.y2 - clip.y1, bg);

                    for (int k = 0; k < order_n; k++) {
                        comp_surface_t* s = &clients[order[k].ci].surfaces[order[k].si];
                        const uint32_t* src = 0;
                        int src_stride = 0;
                        if (s->shadow_valid && s->shadow_active >= 0 && s->shadow_active < COMP_SURFACE_SHADOW_BUFS) {
                            src = s->shadow_pixels[s->shadow_active];
                            src_stride = s->shadow_stride;
                        }
                        if (!src) {
                            src = s->pixels;
                            src_stride = s->stride;
                        }
                        if (!src || src_stride <= 0) continue;
                        blit_surface_clipped(out, stride, w, h, s->x, s->y, src, src_stride, s->w, s->h, clip);
                    }
                }

                if (!frame_pixels) {
                    if (!rect_empty(&new_preview_rect)) {
                        const int t = 2;
                        draw_frame_rect_clipped(out, stride, w, h, new_preview_rect.x1, new_preview_rect.y1, new_preview_rect.x2 - new_preview_rect.x1, new_preview_rect.y2 - new_preview_rect.y1, t, preview_col, clip);
                    }
                }
            }

            if (frame_pixels) {
                present_damage_to_fb(fb, frame_pixels, stride, &dmg);

                for (int ri = 0; ri < dmg.n; ri++) {
                    const comp_rect_t clip = dmg.rects[ri];
                    if (rect_empty(&clip)) continue;

                    if (!rect_empty(&new_preview_rect)) {
                        const int t = 2;
                        draw_frame_rect_clipped(fb, stride, w, h, new_preview_rect.x1, new_preview_rect.y1, new_preview_rect.x2 - new_preview_rect.x1, new_preview_rect.y2 - new_preview_rect.y1, t, preview_col, clip);
                    }
                }
            }
        }

        if (cursor_moved || dmg.n > 0) {
            comp_cursor_save_under_draw(fb, stride, w, h, ms.x, ms.y);
            draw_mx = ms.x;
            draw_my = ms.y;
        }

        first_frame = 0;

        usleep(16000);
    }

    close(fd_mouse);

    if (frame_pixels && frame_size_bytes) {
        munmap((void*)frame_pixels, frame_size_bytes);
        frame_pixels = 0;
    }
    if (frame_shm_fd >= 0) {
        close(frame_shm_fd);
        frame_shm_fd = -1;
    }

    if (clients) {
        for (int i = 0; i < clients_cap; i++) {
            if (clients[i].connected) {
                comp_client_disconnect(&clients[i]);
            }
        }
        free(clients);
        clients = 0;
        clients_cap = 0;
    }
    if (prev_state) {
        free(prev_state);
        prev_state = 0;
        prev_state_cap_clients = 0;
    }
    if (order) {
        free(order);
        order = 0;
        order_cap = 0;
    }

    if (wm_pid > 0) {
        (void)syscall(9, wm_pid, 0, 0);
        wm_pid = -1;
    }

    comp_buffer_destroy(&buf);

    if (!g_fb_released) {
        fb_release();
        g_fb_released = 1;
    }

    return 0;
}
