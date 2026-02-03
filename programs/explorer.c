// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Yula1234

#include <yula.h>
#include <comp.h>
#include <font.h>

static int WIN_W = 640;
static int WIN_H = 480;

#define C_WIN_BG      0x1E1E1E
#define C_SIDEBAR     0x252526
#define C_HEADER      0x2D2D2D
#define C_SELECTION   0x094771
#define C_BORDER      0x3E3E42
#define C_TEXT        0xCCCCCC
#define C_TEXT_DIM    0x858585
#define C_ACCENT      0x007ACC

#define C_FLD_DARK    0xC9A43E
#define C_FLD_LIGHT   0xE8C660
#define C_FILE_BODY   0xF0F0F0
#define C_FILE_FOLD   0xCCD0D0
#define C_EXE_ACCENT  0x4EC9B0
#define C_ASM_ACCENT  0xCE9178

#define ICON_W 48
#define ICON_H 40
#define GRID_X 20
#define GRID_Y 60
#define GAP_X  32 
#define GAP_Y  30

typedef struct {
    char name[64];
    int type; // 1=File, 2=Dir, 3=Exe, 4=Asm
    int size;
    int x, y;
    int hover;
} FileEntry;

FileEntry entries[256];
int entry_count = 0;
char current_path[256];
int selected_idx = -1;
uint32_t* canvas = 0;

static const uint32_t surface_id = 1u;

static comp_conn_t conn;
static char shm_name[32];
static int shm_fd = -1;
static int shm_gen = 0;
static uint32_t size_bytes = 0;

void sys_unlink(const char* path) {
    __asm__ volatile("int $0x80" : : "a"(14), "b"((int)path));
}

static int ensure_surface(uint32_t need_w, uint32_t need_h) {
    if (need_w == 0 || need_h == 0) return -1;

    uint64_t bytes64 = (uint64_t)need_w * (uint64_t)need_h * 4ull;
    if (bytes64 == 0 || bytes64 > 0xFFFFFFFFu) return -1;
    const uint32_t need_bytes = (uint32_t)bytes64;

    const int can_reuse = (canvas && shm_fd >= 0 && shm_name[0] != '\0' && need_bytes <= size_bytes);
    if (can_reuse) {
        uint16_t err = 0;
        if (comp_send_attach_shm_name_sync(&conn, surface_id, shm_name, size_bytes, need_w, need_h, need_w, 0u, 2000u, &err) != 0) {
            return -1;
        }
        return 0;
    }

    uint64_t grow64 = (uint64_t)size_bytes * 2ull;
    uint64_t cap64 = (grow64 >= (uint64_t)need_bytes) ? grow64 : (uint64_t)need_bytes;
    if (cap64 > 0xFFFFFFFFu) cap64 = (uint64_t)need_bytes;
    const uint32_t cap_bytes = (uint32_t)cap64;

    char new_name[32];
    new_name[0] = '\0';
    int new_fd = -1;
    for (int i = 0; i < 16; i++) {
        shm_gen++;
        (void)snprintf(new_name, sizeof(new_name), "explorer_%d_r%d", getpid(), shm_gen);
        new_fd = shm_create_named(new_name, cap_bytes);
        if (new_fd >= 0) break;
    }
    if (new_fd < 0) return -1;

    uint32_t* new_canvas = (uint32_t*)mmap(new_fd, cap_bytes, MAP_SHARED);
    if (!new_canvas) {
        close(new_fd);
        shm_unlink_named(new_name);
        return -1;
    }

    uint16_t err = 0;
    if (comp_send_attach_shm_name_sync(&conn, surface_id, new_name, cap_bytes, need_w, need_h, need_w, 0u, 2000u, &err) != 0) {
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

    if (old_canvas) munmap((void*)old_canvas, old_size_bytes);
    if (old_fd >= 0) close(old_fd);
    if (old_name[0]) shm_unlink_named(old_name);
    return 0;
}

void fmt_int(int n, char* buf) {
    if (n == 0) { buf[0] = '0'; buf[1] = 0; return; }
    int i = 0; int t = n;
    while (t > 0) { t /= 10; i++; }
    buf[i] = 0;
    while (n > 0) { buf[--i] = (n % 10) + '0'; n /= 10; }
}

uint32_t blend(uint32_t fg, uint32_t bg, uint8_t alpha) {
    if (alpha == 255) return fg;
    if (alpha == 0) return bg;
    uint32_t r = ((fg >> 16) & 0xFF) * alpha + ((bg >> 16) & 0xFF) * (255 - alpha);
    uint32_t g = ((fg >> 8) & 0xFF) * alpha + ((bg >> 8) & 0xFF) * (255 - alpha);
    uint32_t b = (fg & 0xFF) * alpha + (bg & 0xFF) * (255 - alpha);
    return ((r >> 8) << 16) | ((g >> 8) << 8) | (b >> 8);
}

void put_pixel_alpha(int x, int y, uint32_t color, uint8_t alpha) {
    if (x >= 0 && x < WIN_W && y >= 0 && y < WIN_H) {
        uint32_t bg = canvas[y * WIN_W + x];
        canvas[y * WIN_W + x] = blend(color, bg, alpha);
    }
}

void fill_rect(int x, int y, int w, int h, uint32_t color) {
    for (int j = 0; j < h; j++) {
        int py = y + j;
        if (py < 0 || py >= WIN_H) continue;
        for (int i = 0; i < w; i++) {
            int px = x + i;
            if (px >= 0 && px < WIN_W) canvas[py * WIN_W + px] = color;
        }
    }
}

void fill_rect_grad(int x, int y, int w, int h, uint32_t c1, uint32_t c2) {
    for (int j = 0; j < h; j++) {
        int py = y + j;
        if (py < 0 || py >= WIN_H) continue;
        uint8_t r = (((c1 >> 16) & 0xFF) * (h - j) + ((c2 >> 16) & 0xFF) * j) / h;
        uint8_t g = (((c1 >> 8) & 0xFF) * (h - j) + ((c2 >> 8) & 0xFF) * j) / h;
        uint8_t b = ((c1 & 0xFF) * (h - j) + (c2 & 0xFF) * j) / h;
        uint32_t col = (r << 16) | (g << 8) | b;
        for (int i = 0; i < w; i++) {
            int px = x + i;
            if (px >= 0 && px < WIN_W) canvas[py * WIN_W + px] = col;
        }
    }
}

void fill_rect_alpha(int x, int y, int w, int h, uint32_t color, uint8_t alpha) {
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) put_pixel_alpha(x + i, y + j, color, alpha);
    }
}

void draw_frame(int x, int y, int w, int h, uint32_t color) {
    fill_rect(x, y, w, 1, color);
    fill_rect(x, y + h - 1, w, 1, color);
    fill_rect(x, y, 1, h, color);
    fill_rect(x + w - 1, y, 1, h, color);
}

void draw_icon_folder(int x, int y) {
    fill_rect(x + 2, y, 16, 6, C_FLD_DARK);
    fill_rect_grad(x, y + 4, 40, 28, C_FLD_LIGHT, C_FLD_DARK);
    draw_frame(x, y + 4, 40, 28, 0x8A7010);
    fill_rect(x + 1, y + 5, 38, 1, 0xFFE080);
}

void draw_icon_file(int x, int y, int type) {
    int w = 32; int h = 38; int x_off = 4;
    fill_rect(x + x_off, y, w, h, C_FILE_BODY);
    int fold = 8;
    for(int i=0; i<fold; i++) {
        fill_rect(x + x_off + w - fold + i, y, 1, i + 1, C_WIN_BG);
        fill_rect(x + x_off + w - fold + i, y + fold - i - 1, 1, i + 1, C_FILE_FOLD);
    }
    fill_rect_alpha(x + x_off + w, y + 2, 2, h - 2, 0x000000, 60);
    fill_rect_alpha(x + x_off + 2, y + h, w - 2, 2, 0x000000, 60);
    uint32_t line_col = 0xAAAAAA;
    for (int i = 0; i < 4; i++) fill_rect(x + x_off + 6, y + 10 + (i * 5), w - 12, 2, line_col);
    uint32_t badge_col = 0x888888;
    if (type == 3) badge_col = C_EXE_ACCENT;
    if (type == 4) badge_col = C_ASM_ACCENT;
    if (type != 1) fill_rect(x + x_off + 4, y + 25, w - 8, 6, badge_col);
}

void load_directory() {
    entry_count = 0;
    selected_idx = -1;
    
    int fd = open(current_path, 0);
    if (fd < 0) return;

    struct { uint32_t inode; char name[60]; } ent;
    while (read(fd, &ent, 64) > 0) {
        if (ent.inode == 0) continue;
        if (strcmp(ent.name, ".") == 0) continue;
        
        FileEntry* e = &entries[entry_count];
        strcpy(e->name, ent.name);
        
        char full[300];
        strcpy(full, current_path);
        if (full[strlen(full)-1] != '/') strcat(full, "/");
        strcat(full, ent.name);

        stat_t st;
        if (stat(full, &st) == 0) {
            e->type = st.type;
            e->size = st.size;
        } else {
            e->type = 1; e->size = 0;
        }

        if (e->type == 1) {
            int len = strlen(e->name);
            if (len > 4 && strcmp(e->name + len - 4, ".exe") == 0) e->type = 3;
            else if (len > 4 && strcmp(e->name + len - 4, ".asm") == 0) e->type = 4;
        }
        
        entry_count++;
        if (entry_count >= 256) break;
    }
    close(fd);
}

void nav_up() {
    if (strcmp(current_path, "/") == 0) return;
    int len = strlen(current_path);
    while (len > 0 && current_path[len-1] != '/') len--;
    if (len > 1 && current_path[len-1] == '/') len--;
    current_path[len] = 0;
    if (len == 0) strcpy(current_path, "/");
    load_directory();
}

void render_all() {
    fill_rect(0, 0, WIN_W, WIN_H, C_WIN_BG);
    
    fill_rect(0, 0, WIN_W, 36, C_HEADER);
    fill_rect(0, 36, WIN_W, 1, 0x000000);
    
    fill_rect(50, 6, WIN_W - 60, 24, 0x181818);
    draw_frame(50, 6, WIN_W - 60, 24, C_BORDER);
    draw_string(canvas, WIN_W, WIN_H, 60, 14, current_path, C_TEXT);

    int bx = 15, by = 12;
    fill_rect(bx, by + 4, 12, 4, C_TEXT);
    fill_rect(bx, by + 4, 2, 4, C_TEXT);
    fill_rect(bx + 2, by + 2, 2, 8, C_TEXT);
    fill_rect(bx + 4, by, 2, 12, C_TEXT);

    int bar_y = WIN_H - 24;
    fill_rect(0, bar_y, WIN_W, 24, C_HEADER);
    fill_rect(0, bar_y, WIN_W, 1, C_BORDER);

    fs_info_t fs;
    if (get_fs_info(&fs) == 0 && fs.total_blocks > 0) {
        uint32_t used = fs.total_blocks - fs.free_blocks;
        uint32_t pct = (used * 100) / fs.total_blocks;
        int bar_w = 100; int bar_h = 10;
        int bx = WIN_W - bar_w - 10; int by = WIN_H - 17;
        fill_rect(bx, by, bar_w, bar_h, 0x111111);
        uint32_t col = (pct > 80) ? 0xC94E4E : C_ACCENT;
        fill_rect(bx + 1, by + 1, (pct * (bar_w - 2)) / 100, bar_h - 2, col);
        
        draw_string(canvas, WIN_W, WIN_H, bx - 70, by + 1, "Storage:", C_TEXT_DIM);
        char buf[32], num[10]; fmt_int(entry_count, num);
        strcpy(buf, "Items: "); strcat(buf, num);
        draw_string(canvas, WIN_W, WIN_H, 10, by + 1, buf, C_TEXT_DIM);
    }

    int cur_x = GRID_X;
    int cur_y = GRID_Y;

    for (int i = 0; i < entry_count; i++) {
        FileEntry* e = &entries[i];
        e->x = cur_x;
        e->y = cur_y;
        
        int hit_w = ICON_W + 20;
        int hit_h = ICON_H + 30;
        int hit_x = cur_x - 10;
        int hit_y = cur_y - 5;

        if (i == selected_idx) {
            fill_rect_alpha(hit_x, hit_y, hit_w, hit_h, C_SELECTION, 100);
            draw_frame(hit_x, hit_y, hit_w, hit_h, C_ACCENT);
        } else if (e->hover) {
            fill_rect_alpha(hit_x, hit_y, hit_w, hit_h, 0xFFFFFF, 20);
        }

        if (e->type == 2) draw_icon_folder(cur_x, cur_y);
        else draw_icon_file(cur_x, cur_y, e->type);

        char name_short[16];
        int len = strlen(e->name);
        if (len > 9) {
            memcpy(name_short, e->name, 7);
            name_short[7] = '.'; name_short[8] = '.'; name_short[9] = 0;
        } else {
            strcpy(name_short, e->name);
        }
        
        int text_w = strlen(name_short) * 8;
        int text_x = cur_x + (40 - text_w) / 2;
        
        draw_string(canvas, WIN_W, WIN_H, text_x + 1, cur_y + ICON_H + 6, name_short, 0x000000);
        draw_string(canvas, WIN_W, WIN_H, text_x, cur_y + ICON_H + 5, name_short, C_TEXT);

        cur_x += ICON_W + GAP_X;
        if (cur_x + ICON_W > WIN_W - GRID_X) {
            cur_x = GRID_X;
            cur_y += ICON_H + GAP_Y;
        }
    }
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    set_term_mode(0);
    strcpy(current_path, "/");
    load_directory();

    comp_conn_reset(&conn);
    if (comp_connect(&conn, "flux") != 0) return 1;
    if (comp_send_hello(&conn) != 0) {
        comp_disconnect(&conn);
        return 1;
    }

    shm_name[0] = '\0';
    size_bytes = (uint32_t)WIN_W * (uint32_t)WIN_H * 4u;
    for (int i = 0; i < 8; i++) {
        (void)snprintf(shm_name, sizeof(shm_name), "explorer_%d_%d", getpid(), i);
        shm_fd = shm_create_named(shm_name, size_bytes);
        if (shm_fd >= 0) break;
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
        if (comp_send_attach_shm_name_sync(&conn, surface_id, shm_name, size_bytes, (uint32_t)WIN_W, (uint32_t)WIN_H, (uint32_t)WIN_W, 0u, 2000u, &err) != 0) {
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

    render_all();
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

    int running = 1;
    int have_mouse = 0;
    int last_mx = 0;
    int last_my = 0;
    int last_buttons = 0;

    while (running) {
        int need_update = 0;

        for (;;) {
            int rr = comp_try_recv(&conn, &hdr, payload, (uint32_t)sizeof(payload));
            if (rr < 0) {
                running = 0;
                break;
            }
            if (rr == 0) break;

            if (hdr.type != (uint16_t)COMP_IPC_MSG_INPUT || hdr.len != (uint32_t)sizeof(comp_ipc_input_t)) {
                continue;
            }

            comp_ipc_input_t in;
            memcpy(&in, payload, sizeof(in));
            if (in.surface_id != surface_id) continue;

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

                if (mx != last_mx || my != last_my) {
                    int hover_changed = 0;
                    for (int i = 0; i < entry_count; i++) {
                        int prev = entries[i].hover;
                        int x = entries[i].x - 10;
                        int y = entries[i].y - 5;
                        int w = ICON_W + 20;
                        int h = ICON_H + 30;

                        entries[i].hover = (mx >= x && mx < x + w && my >= y && my < y + h);
                        if (prev != entries[i].hover) hover_changed = 1;
                    }
                    if (hover_changed) need_update = 1;
                }

                if (down_now && !down_prev) {
                    if (my < 36) {
                        if (mx < 50) {
                            nav_up();
                            need_update = 1;
                        }
                    } else {
                        int hit = -1;
                        for (int i = 0; i < entry_count; i++) {
                            if (entries[i].hover) {
                                hit = i;
                                break;
                            }
                        }

                        if (hit != -1) {
                            if (hit == selected_idx) {
                                if (entries[hit].type == 2) {
                                    if (strcmp(entries[hit].name, "..") == 0) {
                                        nav_up();
                                    } else {
                                        if (strcmp(current_path, "/") != 0) strcat(current_path, "/");
                                        strcat(current_path, entries[hit].name);
                                        load_directory();
                                    }
                                }
                            } else {
                                selected_idx = hit;
                            }
                        } else {
                            selected_idx = -1;
                        }
                        need_update = 1;
                    }
                }

                last_mx = mx;
                last_my = my;
                last_buttons = buttons;
                continue;
            }

            if (in.kind == COMP_IPC_INPUT_KEY) {
                if (in.key_state != 1u) continue;
                const unsigned char c = (unsigned char)(uint8_t)in.keycode;
                if (c == 'd' || c == 'D') {
                    if (selected_idx != -1 && entries[selected_idx].type != 2) {
                        char full[300];
                        strcpy(full, current_path);
                        if (full[strlen(full) - 1] != '/') strcat(full, "/");
                        strcat(full, entries[selected_idx].name);
                        sys_unlink(full);
                        load_directory();
                        need_update = 1;
                    }
                }
                continue;
            }

            if (in.kind == COMP_IPC_INPUT_CLOSE) {
                running = 0;
                break;
            }

            if (in.kind == COMP_IPC_INPUT_RESIZE) {
                const int32_t nw = in.x;
                const int32_t nh = in.y;
                if (nw <= 0 || nh <= 0) continue;
                if (nw == WIN_W && nh == WIN_H) continue;

                if (ensure_surface((uint32_t)nw, (uint32_t)nh) != 0) {
                    continue;
                }
                WIN_W = (int)nw;
                WIN_H = (int)nh;
                have_mouse = 0;
                last_buttons = 0;
                need_update = 1;
                continue;
            }
        }

        if (need_update && canvas) {
            render_all();
            if (comp_send_commit(&conn, surface_id, 32, 32, 0u) != 0) {
                running = 0;
            }
        }
        comp_wait_events(&conn, 10000u);
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
    return 0;
}