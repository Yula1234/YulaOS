#include "flux_internal.h"

#include "flux_gpu_present.h"

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

static fb_rect_t fb_rect_make(int32_t x, int32_t y, int32_t w, int32_t h) {
    fb_rect_t r;
    r.x = x;
    r.y = y;
    r.w = w;
    r.h = h;
    return r;
}

static fb_rect_t fb_rect_from_comp(comp_rect_t r) {
    return fb_rect_make((int32_t)r.x1, (int32_t)r.y1, (int32_t)(r.x2 - r.x1), (int32_t)(r.y2 - r.y1));
}

static int is_wm_reserved_key(uint8_t kc) {
    if (kc == 0xC0u || kc == 0xC1u) return 1;
    if (kc >= 0x90u && kc <= 0x95u) return 1;
    if (kc >= 0xA0u && kc <= 0xAFu) return 1;
    if (kc >= 0xB1u && kc <= 0xB4u) return 1;
    return 0;
}

static void comp_disconnect_client_with_wm(comp_client_t* clients, int clients_cap, int dc, wm_conn_t* wm, comp_input_state_t* input, comp_preview_t* preview, int* preview_dirty) {
    if (dc < 0 || dc >= clients_cap) return;
    if (!clients[dc].connected) return;

    dbg_write("flux: client disconnected\n");

    if (wm && wm->connected) {
        for (int si = 0; si < COMP_MAX_SURFACES; si++) {
            comp_surface_t* s = &clients[dc].surfaces[si];
            if (!s->in_use) continue;
            comp_ipc_wm_event_t ev;
            memset(&ev, 0, sizeof(ev));
            ev.kind = COMP_WM_EVENT_UNMAP;
            ev.client_id = (uint32_t)dc;
            ev.surface_id = s->id;
            ev.flags = 0;
            if (wm_send_event(wm, &ev, 1) < 0) {
                wm_disconnect(wm);
                if (input) {
                    input->focus_client = -1;
                    input->focus_surface_id = 0;
                    input->wm_pointer_grab_active = 0;
                    input->wm_pointer_grab_client = -1;
                    input->wm_pointer_grab_surface_id = 0;
                    input->wm_keyboard_grab_active = 0;
                }
                if (preview && preview_dirty) {
                    if (preview->active) {
                        preview->active = 0;
                        *preview_dirty = 1;
                    }
                }
                break;
            }
        }
    }

    comp_client_disconnect(&clients[dc]);
}

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
                                int* order_cap,
                                flux_gpu_comp_surface_t** comp_surfaces,
                                int* comp_surfaces_cap) {
    if (!clients || !clients_cap || !prev_state || !prev_state_cap_clients || !order || !order_cap || !comp_surfaces || !comp_surfaces_cap) return -1;
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
    size_t new_comp_bytes = new_order_n * sizeof(flux_gpu_comp_surface_t);

    comp_client_t* new_clients = (comp_client_t*)malloc(new_clients_bytes);
    draw_surface_state_t* new_prev = (draw_surface_state_t*)malloc(new_prev_bytes);
    draw_item_t* new_order = (draw_item_t*)malloc(new_order_bytes);
    flux_gpu_comp_surface_t* new_comp = (flux_gpu_comp_surface_t*)malloc(new_comp_bytes);

    if (!new_clients || !new_prev || !new_order || !new_comp) {
        if (new_clients) free(new_clients);
        if (new_prev) free(new_prev);
        if (new_order) free(new_order);
        if (new_comp) free(new_comp);
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

    memset(new_comp, 0, new_comp_bytes);

    if (*clients) free(*clients);
    if (*prev_state) free(*prev_state);
    if (*order) free(*order);
    if (*comp_surfaces) free(*comp_surfaces);

    *clients = new_clients;
    *clients_cap = new_cap;
    *prev_state = new_prev;
    *prev_state_cap_clients = new_cap;
    *order = new_order;
    *order_cap = (int)new_order_n;
    *comp_surfaces = new_comp;
    *comp_surfaces_cap = (int)new_order_n;
    return 0;
}

__attribute__((force_align_arg_pointer)) int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    dbg_write("flux: enter main\n");

    set_term_mode(0);

    dbg_write("flux: install signals\n");
    signal(2, (void*)on_sigint_ignore);
    signal(15, (void*)on_signal);
    dbg_write("flux: signals ok\n");

    dbg_write("flux: open /dev/fb0\n");
    int fd_fb = open("/dev/fb0", 0);
    if (fd_fb < 0) {
        dbg_write("flux: cannot open /dev/fb0\n");
        return 1;
    }

    dbg_write("flux: read fb info\n");
    fb_info_t info;
    int r = read(fd_fb, &info, sizeof(info));
    close(fd_fb);
    dbg_write("flux: fb info read done\n");

    if (r < (int)sizeof(info) || info.width == 0 || info.height == 0 || info.pitch == 0) {
        dbg_write("flux: bad fb info\n");
        return 1;
    }

    dbg_write("flux: open /dev/mouse\n");
    int fd_mouse = open("/dev/mouse", 0);
    if (fd_mouse < 0) {
        dbg_write("flux: open mouse failed\n");
        return 1;
    }

    int listen_fd = -1;
    int wm_listen_fd = -1;

    dbg_write("flux: fb_acquire\n");
    if (fb_acquire() != 0) {
        dbg_write("flux: fb busy\n");
        close(fd_mouse);
        return 1;
    }
    dbg_write("flux: fb acquired\n");

    dbg_write("flux: map_framebuffer\n");
    uint32_t* fb = (uint32_t*)map_framebuffer();
    if (!fb) {
        close(fd_mouse);
        fb_release();
        g_fb_released = 1;
        dbg_write("flux: map_framebuffer failed\n");
        return 1;
    }
    dbg_write("flux: fb mapped\n");

    int w = (int)info.width;
    int h = (int)info.height;
    int stride = (int)(info.pitch / 4u);
    if (stride <= 0) stride = w;

    g_screen_w = w;
    g_screen_h = h;

    int frame_shm_fd = -1;
    uint32_t* frame_pixels = 0;
    uint32_t frame_size_bytes = 0;

    flux_gpu_present_t gpu_present;
    memset(&gpu_present, 0, sizeof(gpu_present));
    uint32_t* gpu_pixels = 0;
    int gpu_present_inited = 0;
    int gpu_present_ok = 0;

    if (flux_gpu_present_init(&gpu_present, (uint32_t)w, (uint32_t)h, info.pitch) == 0) {
        gpu_present_inited = 1;
        gpu_pixels = flux_gpu_present_pixels(&gpu_present);
        gpu_present_ok = flux_gpu_present_mode(&gpu_present) != (uint32_t)FLUX_GPU_PRESENT_MODE_NONE;
    }

    if (!gpu_present_ok) {
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

    comp_client_t* clients = 0;
    int clients_cap = 0;
    draw_surface_state_t* prev_state = 0;
    int prev_state_cap_clients = 0;
    draw_item_t* order = 0;
    int order_cap = 0;
    flux_gpu_comp_surface_t* comp_surfaces = 0;
    int comp_surfaces_cap = 0;

    if (comp_clients_reserve(&clients,
                              &clients_cap,
                              (int)COMP_CLIENTS_INIT,
                              &prev_state,
                              &prev_state_cap_clients,
                              &order,
                              &order_cap,
                              &comp_surfaces,
                              &comp_surfaces_cap) != 0) {
        dbg_write("flux: OOM: cannot allocate clients\n");
        close(fd_mouse);
        fb_release();
        g_fb_released = 1;
        return 1;
    }

    comp_input_state_t input;
    comp_input_state_init(&input);

    uint32_t z_counter = 1;

    listen_fd = ipc_listen("flux");
    if (listen_fd < 0) {
        dbg_write("flux: ipc_listen failed\n");
    }

    wm_conn_t wm;
    memset(&wm, 0, sizeof(wm));
    wm.fd_c2s = -1;
    wm.fd_s2c = -1;
    wm.connected = 0;
    ipc_rx_reset(&wm.rx);
    wm.seq_out = 1;

    wm_listen_fd = ipc_listen("flux_wm");
    if (wm_listen_fd < 0) {
        dbg_write("flux: ipc_listen flux_wm failed\n");
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

        {
            const uint32_t pm = gpu_present_ok ? flux_gpu_present_mode(&gpu_present) : (uint32_t)FLUX_GPU_PRESENT_MODE_NONE;
            g_virgl_active = (pm == (uint32_t)FLUX_GPU_PRESENT_MODE_VIRGL_COMPOSE) ? 1 : 0;
        }

        if (listen_fd < 0) {
            listen_fd = ipc_listen("flux");
            if (listen_fd >= 0) {
                dbg_write("flux: ipc_listen flux ok\n");
            }
        }

        if (wm_spawn_retry_wait > 0) wm_spawn_retry_wait--;
        if (!wm.connected && wm_pid > 0) {
            if (wm_spawn_cooldown > 0) {
                wm_spawn_cooldown--;
            } else {
                wm_pid = -1;
            }
        }

        if (wm_listen_fd < 0) {
            wm_listen_fd = ipc_listen("flux_wm");
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
            wargv[0] = (char*)"axwm";
            wm_pid = spawn_process_resolved("axwm", 1, wargv);
            if (wm_pid < 0) {
                dbg_write("flux: spawn axwm failed\n");
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
                input.wm_keyboard_grab_active = 0;
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
                    if (comp_clients_reserve(&clients,
                                              &clients_cap,
                                              want,
                                              &prev_state,
                                              &prev_state_cap_clients,
                                              &order,
                                              &order_cap,
                                              &comp_surfaces,
                                              &comp_surfaces_cap) == 0) {
                        slot = want - 1;
                    }
                }

                if (slot >= 0 && slot < clients_cap) {
                    comp_client_init(&clients[slot], -1, fds[0], fds[1]);
                    dbg_write("flux: accepted client\n");
                } else {
                    dbg_write("flux: reject client (OOM)\n");
                    if (fds[0] >= 0) close(fds[0]);
                    if (fds[1] >= 0) close(fds[1]);
                }
            }
        }

        for (int ci = 0; ci < clients_cap; ci++) {
            if (!clients[ci].connected) continue;
            comp_client_pump(&clients[ci], 0, &z_counter, &wm, (uint32_t)ci, &input);
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
                input.wm_keyboard_grab_active = 0;
                if (preview.active) {
                    preview.active = 0;
                    preview_dirty = 1;
                }
            }
        }

        if (comp_send_mouse(clients, clients_cap, &input, &ms) < 0) {
            const int dc = input.last_client;
            comp_disconnect_client_with_wm(clients, clients_cap, dc, &wm, &input, &preview, &preview_dirty);
        }

        for (;;) {
            char kc = 0;
            int kr = kbd_try_read(&kc);
            if (kr <= 0) break;

            uint8_t kcu = (uint8_t)kc;

            if (kcu == 0x17u || (!wm.connected && kcu == 0x1Bu)) {
                char tmp[64];
                (void)snprintf(tmp, sizeof(tmp), "flux: exit key %u\n", (unsigned)kcu);
                dbg_write(tmp);
                g_should_exit = 1;
                break;
            }

            if (wm.connected) {
                comp_ipc_wm_event_t ev;
                memset(&ev, 0, sizeof(ev));
                ev.kind = COMP_WM_EVENT_KEY;
                ev.client_id = input.focus_client >= 0 ? (uint32_t)input.focus_client : COMP_WM_CLIENT_NONE;
                ev.surface_id = input.focus_surface_id;
                ev.keycode = (uint32_t)kcu;
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
                    input.wm_pointer_grab_active = 0;
                    input.wm_pointer_grab_client = -1;
                    input.wm_pointer_grab_surface_id = 0;
                    input.wm_keyboard_grab_active = 0;
                }
            }

            if (wm.connected && is_wm_reserved_key(kcu)) {
                continue;
            }

            if (comp_send_key(clients, clients_cap, &input, (uint32_t)kcu, 1u) < 0) {
                int dc = input.focus_client;
                if (dc >= 0 && dc < clients_cap && clients[dc].connected) {
                    comp_disconnect_client_with_wm(clients, clients_cap, dc, &wm, &input, &preview, &preview_dirty);
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
                input.wm_keyboard_grab_active = 0;
                if (preview.active) {
                    preview.active = 0;
                    preview_dirty = 1;
                }
            }
        }

        if (g_should_exit) {
            break;
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
                    if (prev->valid) damage_add(&dmg, rect_make(prev->x - 1, prev->y - 1, prev->w + 2, prev->h + 2), w, h);
                    if (cur.valid) damage_add(&dmg, rect_make(cur.x - 1, cur.y - 1, cur.w + 2, cur.h + 2), w, h);
                }

                prev_state[idx] = cur;
            }
        }

        const uint32_t present_mode = gpu_present_ok ? flux_gpu_present_mode(&gpu_present) : (uint32_t)FLUX_GPU_PRESENT_MODE_NONE;
        int virgl_mode = gpu_present_ok && present_mode == (uint32_t)FLUX_GPU_PRESENT_MODE_VIRGL_COMPOSE;

        uint32_t* front = (gpu_present_ok && !virgl_mode) ? gpu_pixels : fb;

        const int cursor_moved = ((int32_t)ms.x != draw_mx || (int32_t)ms.y != draw_my);
        if (!virgl_mode) {
            if (cursor_moved || dmg.n > 0) {
                comp_cursor_restore(front, stride, w, h);
            }
        }

        if (!virgl_mode && dmg.n > 0) {
            preview_dirty = 0;
            prev_preview_rect = new_preview_rect;

            uint32_t bg = 0x101010;
            uint32_t* out = frame_pixels ? frame_pixels : front;

            int order_n = 0;
            int connected_clients = 0;
            for (int ci = 0; ci < clients_cap; ci++) {
                if (!clients[ci].connected) continue;
                connected_clients++;
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

                        uint32_t border_col = 0x808080u;
                        if (input.focus_client == (int)order[k].ci && input.focus_surface_id == s->id) {
                            border_col = 0x007ACCu;
                        }
                        draw_frame_rect_clipped(out, stride, w, h, s->x - 1, s->y - 1, s->w + 2, s->h + 2, 1, border_col, clip);
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

        int32_t prev_draw_mx = draw_mx;
        int32_t prev_draw_my = draw_my;

        if (!virgl_mode) {
            if (cursor_moved || dmg.n > 0) {
                comp_cursor_save_under_draw(front, stride, w, h, (int)ms.x, (int)ms.y);
                draw_mx = (int32_t)ms.x;
                draw_my = (int32_t)ms.y;
            }
        }

        if (cursor_moved || dmg.n > 0) {
            fb_rect_t rects[COMP_MAX_DAMAGE_RECTS + 2];
            uint32_t rect_n = 0;

            for (int ri = 0; ri < dmg.n && rect_n < (uint32_t)COMP_MAX_DAMAGE_RECTS; ri++) {
                const comp_rect_t clip = dmg.rects[ri];
                if (rect_empty(&clip)) continue;
                rects[rect_n++] = fb_rect_from_comp(clip);
            }

            if (!virgl_mode) {
                if (prev_draw_mx != 0x7FFFFFFF && prev_draw_my != 0x7FFFFFFF) {
                    comp_rect_t old_cursor = rect_make((int)prev_draw_mx - COMP_CURSOR_SAVE_HALF,
                                                       (int)prev_draw_my - COMP_CURSOR_SAVE_HALF,
                                                       COMP_CURSOR_SAVE_W,
                                                       COMP_CURSOR_SAVE_H);
                    old_cursor = rect_clip_to_screen(old_cursor, w, h);
                    if (!rect_empty(&old_cursor) && rect_n < (uint32_t)(COMP_MAX_DAMAGE_RECTS + 2)) {
                        rects[rect_n++] = fb_rect_from_comp(old_cursor);
                    }
                }

                comp_rect_t new_cursor = rect_make((int)ms.x - COMP_CURSOR_SAVE_HALF,
                                                   (int)ms.y - COMP_CURSOR_SAVE_HALF,
                                                   COMP_CURSOR_SAVE_W,
                                                   COMP_CURSOR_SAVE_H);
                new_cursor = rect_clip_to_screen(new_cursor, w, h);
                if (!rect_empty(&new_cursor) && rect_n < (uint32_t)(COMP_MAX_DAMAGE_RECTS + 2)) {
                    rects[rect_n++] = fb_rect_from_comp(new_cursor);
                }
            } else {
                if (prev_draw_mx != 0x7FFFFFFF && prev_draw_my != 0x7FFFFFFF) {
                    comp_rect_t old_cursor = rect_make((int)prev_draw_mx,
                                                       (int)prev_draw_my,
                                                       COMP_CURSOR_SAVE_W,
                                                       COMP_CURSOR_SAVE_H);
                    old_cursor = rect_intersect(old_cursor, rect_make(0, 0, w, h));
                    if (!rect_empty(&old_cursor)) {
                        rects[rect_n++] = fb_rect_from_comp(old_cursor);
                    }
                }

                comp_rect_t new_cursor = rect_make((int)ms.x, (int)ms.y, COMP_CURSOR_SAVE_W, COMP_CURSOR_SAVE_H);
                new_cursor = rect_intersect(new_cursor, rect_make(0, 0, w, h));
                if (!rect_empty(&new_cursor)) {
                    rects[rect_n++] = fb_rect_from_comp(new_cursor);
                }
            }

            if (gpu_present_ok) {
                if (virgl_mode) {
                    if (comp_surfaces && comp_surfaces_cap > 0) {
                        uint32_t comp_n = 0u;
                        int order_n = 0;
                        for (int ci = 0; ci < clients_cap; ci++) {
                            if (!clients[ci].connected) continue;
                            for (int si = 0; si < COMP_MAX_SURFACES; si++) {
                                comp_surface_t* s = &clients[ci].surfaces[si];
                                if (!s->in_use || !s->attached || !s->committed) continue;
                                if (s->id == 0u) continue;
                                if (s->w <= 0 || s->h <= 0) continue;
                                if (order_n >= order_cap) break;
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

                        for (int i = 0; i < order_n; i++) {
                            if (comp_n >= (uint32_t)comp_surfaces_cap) break;
                            comp_surface_t* s = &clients[order[i].ci].surfaces[order[i].si];

                            int shm_fd = -1;
                            uint32_t shm_size_bytes = 0u;
                            uint32_t stride_bytes = 0u;

                            if (s->owns_buffer && s->shm_fd >= 0 && s->size_bytes != 0u && s->stride > 0) {
                                shm_fd = s->shm_fd;
                                shm_size_bytes = s->size_bytes;
                                stride_bytes = (uint32_t)s->stride * 4u;
                            }

                            if (shm_fd < 0 || shm_size_bytes == 0u || stride_bytes == 0u) continue;

                            flux_gpu_comp_surface_t* cs = &comp_surfaces[comp_n++];
                            cs->client_id = (uint32_t)order[i].ci;
                            cs->surface_id = s->id;
                            cs->x = (int32_t)s->x;
                            cs->y = (int32_t)s->y;
                            cs->width = (uint32_t)s->w;
                            cs->height = (uint32_t)s->h;
                            cs->stride_bytes = stride_bytes;
                            cs->shm_size_bytes = shm_size_bytes;
                            cs->shm_fd = shm_fd;
                            cs->commit_gen = s->commit_gen;

                            cs->flags = 0u;
                            if (input.focus_client == (int)order[i].ci && input.focus_surface_id == s->id) {
                                cs->flags |= FLUX_GPU_SURFACE_FLAG_ACTIVE;
                            }

                            cs->damage_count = 0u;
                            cs->damage = 0;
                            if (s->damage_committed_gen == s->commit_gen && s->damage_committed_count) {
                                cs->damage_count = s->damage_committed_count;
                                cs->damage = s->damage_committed;
                            }
                        }

                        fb_rect_t preview_fb;
                        fb_rect_t* preview_ptr = 0;
                        if (!rect_empty(&new_preview_rect)) {
                            preview_fb = fb_rect_from_comp(new_preview_rect);
                            preview_ptr = &preview_fb;
                        }

                        preview_dirty = 0;
                        prev_preview_rect = new_preview_rect;

                        if (flux_gpu_present_compose(&gpu_present,
                                                      rects,
                                                      rect_n,
                                                      comp_surfaces,
                                                      comp_n,
                                                      preview_ptr,
                                                      (int32_t)ms.x,
                                                      (int32_t)ms.y) != 0) {
                            comp_cursor_reset();
                            draw_mx = 0x7FFFFFFF;
                            draw_my = 0x7FFFFFFF;
                            if (gpu_present_inited) {
                                flux_gpu_present_shutdown(&gpu_present);
                                gpu_present_inited = 0;
                            }
                            gpu_present_ok = 0;
                            gpu_pixels = 0;
                            virgl_mode = 0;

                            fill_rect(fb, stride, w, h, 0, 0, w, h, 0x101010u);
                            for (uint32_t si = 0; si < comp_n; si++) {
                                const flux_gpu_comp_surface_t* cs = &comp_surfaces[si];
                                const int ci = (int)order[si].ci;
                                const int sj = (int)order[si].si;
                                if (ci < 0 || ci >= clients_cap) continue;
                                comp_surface_t* s = &clients[ci].surfaces[sj];
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
                                blit_surface_clipped(fb, stride, w, h, s->x, s->y, src, src_stride, s->w, s->h, rect_make(0, 0, w, h));

                                uint32_t border_col = 0x808080u;
                                if (input.focus_client == (int)order[si].ci && input.focus_surface_id == s->id) {
                                    border_col = 0x007ACCu;
                                }
                                draw_frame_rect_clipped(fb, stride, w, h, s->x - 1, s->y - 1, s->w + 2, s->h + 2, 1, border_col, rect_make(0, 0, w, h));
                            }
                            if (!rect_empty(&new_preview_rect)) {
                                draw_frame_rect_clipped(fb,
                                                        stride,
                                                        w,
                                                        h,
                                                        new_preview_rect.x1,
                                                        new_preview_rect.y1,
                                                        new_preview_rect.x2 - new_preview_rect.x1,
                                                        new_preview_rect.y2 - new_preview_rect.y1,
                                                        2,
                                                        0x007ACCu,
                                                        rect_make(0, 0, w, h));
                            }
                            comp_cursor_save_under_draw(fb, stride, w, h, (int)ms.x, (int)ms.y);
                            draw_mx = (int32_t)ms.x;
                            draw_my = (int32_t)ms.y;

                            fb_rect_t full = fb_rect_make(0, 0, w, h);
                            (void)fb_present(fb, info.pitch, &full, 1);
                        } else {
                            draw_mx = (int32_t)ms.x;
                            draw_my = (int32_t)ms.y;
                        }
                    }
                } else if (flux_gpu_present_present(&gpu_present, rects, rect_n) != 0) {
                    uint64_t fb_bytes64 = (uint64_t)info.pitch * (uint64_t)info.height;
                    if (front && fb && front != fb && fb_bytes64 > 0ull && fb_bytes64 <= 0xFFFFFFFFu) {
                        memcpy(fb, front, (size_t)fb_bytes64);
                    }

                    comp_cursor_reset();

                    if (gpu_present_inited) {
                        flux_gpu_present_shutdown(&gpu_present);
                        gpu_present_inited = 0;
                    }

                    gpu_present_ok = 0;
                    gpu_pixels = 0;
                    front = fb;

                    (void)fb_present(fb, info.pitch, rects, rect_n);
                }
            } else {
                (void)fb_present(fb, info.pitch, rects, rect_n);
            }
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

    if (gpu_present_inited) {
        flux_gpu_present_shutdown(&gpu_present);
        gpu_present_inited = 0;
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
    if (comp_surfaces) {
        free(comp_surfaces);
        comp_surfaces = 0;
        comp_surfaces_cap = 0;
    }

    if (wm_pid > 0) {
        (void)syscall(9, wm_pid, 0, 0);
        wm_pid = -1;
    }

    if (!g_fb_released) {
        fb_release();
        g_fb_released = 1;
    }

    return 0;
}
