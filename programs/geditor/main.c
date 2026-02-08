#include "geditor_defs.h"
#include "editor.h"
#include "editor_file.h"
#include "gapbuf.h"
#include "lines.h"
#include "undo.h"
#include "render.h"
#include "ui.h"

static int ensure_surface(uint32_t need_w, uint32_t need_h) {
    if (need_w == 0 || need_h == 0) {
        return -1;
    }

    uint64_t bytes64 = (uint64_t)need_w * (uint64_t)need_h * 4ull;
    if (bytes64 == 0 || bytes64 > 0xFFFFFFFFu) {
        return -1;
    }
    const uint32_t need_bytes = (uint32_t)bytes64;

    const int can_reuse = (canvas && shm_fd >= 0 && shm_name[0] != '\0' && need_bytes <= size_bytes);
    if (can_reuse) {
        uint16_t err = 0;
        int rc = comp_send_attach_shm_name_sync(&conn, surface_id, shm_name, size_bytes, need_w, need_h, need_w, 0u, 2000u, &err);
        if (rc != 0) {
            return -1;
        }
        return 0;
    }

    uint64_t grow64 = (uint64_t)size_bytes * 2ull;
    uint64_t cap64 = (grow64 >= (uint64_t)need_bytes) ? grow64 : (uint64_t)need_bytes;
    if (cap64 > 0xFFFFFFFFu) {
        cap64 = (uint64_t)need_bytes;
    }
    const uint32_t cap_bytes = (uint32_t)cap64;

    char new_name[32];
    new_name[0] = '\0';
    int new_fd = -1;
    for (int i = 0; i < 16; i++) {
        shm_gen++;
        (void)snprintf(new_name, sizeof(new_name), "geditor_%d_r%d", getpid(), shm_gen);
        new_fd = shm_create_named(new_name, cap_bytes);
        if (new_fd >= 0) {
            break;
        }
    }
    if (new_fd < 0) {
        return -1;
    }

    uint32_t* new_canvas = (uint32_t*)mmap(new_fd, cap_bytes, MAP_SHARED);
    if (!new_canvas) {
        close(new_fd);
        shm_unlink_named(new_name);
        return -1;
    }

    uint16_t err = 0;
    int rc = comp_send_attach_shm_name_sync(&conn, surface_id, new_name, cap_bytes, need_w, need_h, need_w, 0u, 2000u, &err);
    if (rc != 0) {
        munmap((void*)new_canvas, cap_bytes);
        close(new_fd);
        shm_unlink_named(new_name);
        return -1;
    }

    uint32_t* old_canvas = canvas;
    uint32_t old_size_bytes = size_bytes;
    int old_fd = shm_fd;
    char old_name[32];
    memcpy(old_name, shm_name, sizeof(old_name));

    canvas = new_canvas;
    size_bytes = cap_bytes;
    shm_fd = new_fd;
    memcpy(shm_name, new_name, sizeof(shm_name));

    if (old_canvas) {
        munmap((void*)old_canvas, old_size_bytes);
    }
    if (old_fd >= 0) {
        close(old_fd);
    }
    if (old_name[0]) {
        shm_unlink_named(old_name);
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        strcpy(ed.filename, "new.asm");
    } else {
        strcpy(ed.filename, argv[1]);
    }

    set_term_mode(0);

    editor_update_lang_from_filename();

    gb_init(&ed.text, 4096);
    lines_init(&ed.lines);
    if (!ed.text.buf) {
        return 1;
    }

    ustack_init(&ed.undo);
    ustack_init(&ed.redo);

    ed.sel_bound = -1;
    ed.dirty = 0;
    ed.is_dragging = 0;
    ed.scroll_y = 0;
    ed.cursor = 0;
    ed.pref_col = 0;
    ed.mode = MODE_EDIT;
    ed.mini_len = 0;
    ed.open_confirm = 0;

    ed.find_len = 0;
    ed.find[0] = 0;

    ed.status_len = 0;
    ed.status[0] = 0;
    ed.status_color = C_UI_MUTED;

    (void)load_file_silent();

    comp_conn_reset(&conn);
    if (comp_connect(&conn, "flux") != 0) {
        return 1;
    }
    if (comp_send_hello(&conn) != 0) {
        comp_disconnect(&conn);
        return 1;
    }

    shm_name[0] = '\0';
    size_bytes = (uint32_t)WIN_W * (uint32_t)WIN_H * 4u;
    for (int i = 0; i < 8; i++) {
        (void)snprintf(shm_name, sizeof(shm_name), "geditor_%d_%d", getpid(), i);
        shm_fd = shm_create_named(shm_name, size_bytes);
        if (shm_fd >= 0) {
            break;
        }
    }
    if (shm_fd < 0) {
        comp_disconnect(&conn);
        return 1;
    }

    canvas = (uint32_t*)mmap(shm_fd, size_bytes, MAP_SHARED);
    if (!canvas) {
        close(shm_fd);
        shm_fd = -1;
        shm_unlink_named(shm_name);
        shm_name[0] = '\0';
        comp_disconnect(&conn);
        return 1;
    }

    {
        uint16_t err = 0;
        int rc = comp_send_attach_shm_name_sync(
            &conn,
            surface_id,
            shm_name,
            size_bytes,
            (uint32_t)WIN_W,
            (uint32_t)WIN_H,
            (uint32_t)WIN_W,
            0u,
            2000u,
            &err
        );
        if (rc != 0) {
            munmap((void*)canvas, size_bytes);
            canvas = 0;
            close(shm_fd);
            shm_fd = -1;
            shm_unlink_named(shm_name);
            shm_name[0] = '\0';
            comp_disconnect(&conn);
            return 1;
        }
    }

    render_editor();
    render_ui();
    if (comp_send_commit(&conn, surface_id, 32, 32, 0u) != 0) {
        (void)comp_send_destroy_surface(&conn, surface_id, 0u);
        munmap((void*)canvas, size_bytes);
        canvas = 0;
        close(shm_fd);
        shm_fd = -1;
        shm_unlink_named(shm_name);
        shm_name[0] = '\0';
        comp_disconnect(&conn);
        return 1;
    }

    comp_ipc_hdr_t hdr;
    uint8_t payload[COMP_IPC_MAX_PAYLOAD];

    int have_mouse = 0;
    int last_mx = 0;
    int last_my = 0;
    int last_buttons = 0;

    while (!ed.quit) {
        int update = 0;
        for (;;) {
            int rr = comp_try_recv(&conn, &hdr, payload, (uint32_t)sizeof(payload));
            if (rr < 0) {
                ed.quit = 1;
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

            if (in.kind == COMP_IPC_INPUT_KEY) {
                if (in.key_state != 1u) {
                    continue;
                }

                unsigned char c = (unsigned char)(uint8_t)in.keycode;
                if (c == 0x15) {
                    save_file();
                    update = 1;
                    continue;
                }
                if (c == 0x1A) {
                    editor_undo();
                    update = 1;
                    continue;
                }
                if (c == 0x19) {
                    editor_redo();
                    update = 1;
                    continue;
                }

                if (ed.mode != MODE_EDIT) {
                    if (c == 0x1B) {
                        ed.mode = MODE_EDIT;
                        ed.open_confirm = 0;
                        update = 1;
                        continue;
                    }
                    if (c == 0x08) {
                        mini_backspace();
                        update = 1;
                        continue;
                    }
                    if (c == 0x0A || c == 0x0D) {
                        if (ed.mode == MODE_FIND) {
                            apply_find_mode();
                        } else if (ed.mode == MODE_GOTO) {
                            apply_goto_mode();
                        } else if (ed.mode == MODE_OPEN) {
                            apply_open_mode();
                        }
                        update = 1;
                        continue;
                    }
                    if (c >= 32 && c <= 126) {
                        mini_putc((char)c);
                        update = 1;
                        continue;
                    }
                    continue;
                }

                if (c == 0x11) {
                    move_left(0);
                } else if (c == 0x12) {
                    move_right(0);
                } else if (c == 0x13) {
                    move_up(0);
                } else if (c == 0x14) {
                    move_down(0);
                } else if (c == 0x82) {
                    move_left(1);
                } else if (c == 0x83) {
                    move_right(1);
                } else if (c == 0x80) {
                    move_up(1);
                } else if (c == 0x81) {
                    move_down(1);
                } else if (c == 0x84) {
                    move_word_left(0);
                } else if (c == 0x85) {
                    move_word_right(0);
                } else if (c == 0x86) {
                    move_word_left(1);
                } else if (c == 0x87) {
                    move_word_right(1);
                } else if (c == 0x08) {
                    backspace();
                } else if (c == 0x0A || c == 0x0D) {
                    editor_insert_newline_autoindent();
                } else if (c == 0x09) {
                    editor_insert_tab_smart();
                } else if (c == 0x03) {
                    copy_selection();
                } else if (c == 0x16) {
                    paste_clipboard();
                } else if (c == 0x06) {
                    enter_find_mode();
                } else if (c == 0x07) {
                    enter_goto_mode();
                } else if (c == 0x0F) {
                    enter_open_mode();
                } else if (c == 0x0E) {
                    if (ed.find_len > 0) {
                        int start = ed.cursor;
                        int text_len = gb_len(&ed.text);
                        if (start < 0) {
                            start = 0;
                        }
                        if (start > text_len) {
                            start = text_len;
                        }
                        if (!editor_find_next_from(start)) {
                            status_set("Not found");
                        }
                    } else {
                        enter_find_mode();
                    }
                } else if (c >= 32 && c <= 126) {
                    insert_char((char)c);
                }

                update = 1;
                continue;
            }

            if (in.kind == COMP_IPC_INPUT_CLOSE) {
                ed.quit = 1;
                update = 1;
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

                const int down_now = ((buttons & 1) != 0);
                const int down_prev = ((prev_buttons & 1) != 0);

                if (down_now && !down_prev) {
                    int pos = get_pos_from_coords(mx, my);
                    ed.cursor = pos;
                    ed.sel_bound = pos;
                    ed.is_dragging = 1;
                    update_pref_col();
                    update = 1;
                }
                if (down_now && ed.is_dragging && (mx != last_mx || my != last_my)) {
                    int pos = get_pos_from_coords(mx, my);
                    if (pos != ed.cursor) {
                        ed.cursor = pos;
                        update_pref_col();
                        update = 1;
                    }
                }
                if (!down_now && down_prev) {
                    ed.is_dragging = 0;
                    if (ed.sel_bound == ed.cursor) {
                        ed.sel_bound = -1;
                    }
                    update = 1;
                }

                last_mx = mx;
                last_my = my;
                last_buttons = buttons;
                continue;
            }

            if (in.kind == COMP_IPC_INPUT_RESIZE) {
                const int32_t nw = in.x;
                const int32_t nh = in.y;
                if (nw <= 0 || nh <= 0) {
                    continue;
                }
                if (nw == WIN_W && nh == WIN_H) {
                    continue;
                }

                if (ensure_surface((uint32_t)nw, (uint32_t)nh) != 0) {
                    continue;
                }

                WIN_W = (int)nw;
                WIN_H = (int)nh;
                have_mouse = 0;
                last_buttons = 0;
                update = 1;
                continue;
            }
        }

        if (update) {
            render_editor();
            render_ui();
            if (comp_send_commit(&conn, surface_id, 32, 32, 0u) != 0) {
                ed.quit = 1;
            }
        }
        comp_wait_events(&conn, 4000u);
    }

    (void)comp_send_destroy_surface(&conn, surface_id, 0u);
    if (canvas && size_bytes) {
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
    comp_disconnect(&conn);

    lines_destroy(&ed.lines);
    gb_destroy(&ed.text);

    ustack_destroy(&ed.undo);
    ustack_destroy(&ed.redo);
    return 0;
}
