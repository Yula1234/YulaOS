#include <yula.h>
#include <comp.h>
#include <font.h>

static inline void dbg_write(const char* s) {
    if (!s) return;
    write(1, s, (uint32_t)strlen(s));
}

static int WIN_W = 520;
static int WIN_H = 260;

#define C_BG        0x1B1B1B
#define C_PANEL_BG  0x202020
#define C_BORDER    0x3E3E42
#define C_TEXT      0xD4D4D4
#define C_MUTED     0x9A9A9A
#define C_ACCENT    0x007ACC
#define C_SELECT_BG 0x094771

#define ROW_H 18
#define PAD_X 12
#define PAD_Y 12

#define MAX_NAME 60

typedef struct {
    char base[MAX_NAME];
    char base_lc[MAX_NAME];
} app_t;

static app_t* g_apps;
static int g_app_count;
static int g_app_cap;

static int* g_filtered;
static int g_filtered_count;
static int g_filtered_cap;

static int g_selected;
static int g_scroll;

static char* g_query;
static char* g_query_lc;
static int g_query_len;
static int g_query_cap;

static const uint32_t surface_id = 1u;

static comp_conn_t conn;
static char shm_name[32];
static int shm_fd = -1;
static int shm_gen;
static uint32_t size_bytes;
static uint32_t* canvas;

static inline int ptr_is_invalid(const void* p) {
    return !p || p == (const void*)-1;
}

static inline int max_i(int a, int b) { return a > b ? a : b; }

static char lower_char(char c) {
    if (c >= 'A' && c <= 'Z') return (char)(c + ('a' - 'A'));
    return c;
}

static int ends_with(const char* s, const char* suf) {
    if (!s || !suf) return 0;
    size_t n = strlen(s);
    size_t m = strlen(suf);
    if (m > n) return 0;
    return strcmp(s + (n - m), suf) == 0;
}

static void apps_reserve(int need) {
    if (need <= g_app_cap) return;
    int cap = (g_app_cap > 0) ? g_app_cap : 64;
    while (cap < need) cap *= 2;
    app_t* next = (app_t*)realloc(g_apps, (size_t)cap * sizeof(*g_apps));
    if (!next) return;
    g_apps = next;
    g_app_cap = cap;
}

static void filtered_reserve(int need) {
    if (need <= g_filtered_cap) return;
    int cap = (g_filtered_cap > 0) ? g_filtered_cap : 64;
    while (cap < need) cap *= 2;
    int* next = (int*)realloc(g_filtered, (size_t)cap * sizeof(*g_filtered));
    if (!next) return;
    g_filtered = next;
    g_filtered_cap = cap;
}

static void query_reserve(int need) {
    if (need <= g_query_cap) return;
    int cap = (g_query_cap > 0) ? g_query_cap : 32;
    while (cap < need) cap *= 2;

    char* nq = (char*)malloc((size_t)cap);
    char* nl = (char*)malloc((size_t)cap);
    if (!nq || !nl) {
        if (nq) free(nq);
        if (nl) free(nl);
        return;
    }

    const int copy_n = (g_query && g_query_lc && g_query_cap > 0) ? ((g_query_len + 1 < g_query_cap) ? (g_query_len + 1) : g_query_cap) : 1;
    if (g_query && copy_n > 0) memcpy(nq, g_query, (size_t)copy_n);
    else nq[0] = '\0';

    if (g_query_lc && copy_n > 0) memcpy(nl, g_query_lc, (size_t)copy_n);
    else nl[0] = '\0';

    if (g_query) free(g_query);
    if (g_query_lc) free(g_query_lc);
    g_query = nq;
    g_query_lc = nl;
    g_query_cap = cap;
}

static void apps_push_base(const char* base) {
    if (!base || !*base) return;
    apps_reserve(g_app_count + 1);
    if (!g_apps || g_app_count >= g_app_cap) return;

    (void)snprintf(g_apps[g_app_count].base, sizeof(g_apps[g_app_count].base), "%s", base);
    for (int i = 0; g_apps[g_app_count].base[i] && i < (int)sizeof(g_apps[g_app_count].base_lc) - 1; i++) {
        g_apps[g_app_count].base_lc[i] = lower_char(g_apps[g_app_count].base[i]);
        g_apps[g_app_count].base_lc[i + 1] = '\0';
    }
    g_app_count++;
}

static void apps_swap_idx(int a, int b) {
    if (a == b) return;
    app_t tmp = g_apps[a];
    g_apps[a] = g_apps[b];
    g_apps[b] = tmp;
}

static void apps_sort_unique(void) {
    if (!g_apps || g_app_count <= 1) return;

    struct {
        int lo;
        int hi;
    } stack[64];

    int sp = 0;
    stack[sp].lo = 0;
    stack[sp].hi = g_app_count - 1;
    sp++;

    while (sp > 0) {
        sp--;
        int lo = stack[sp].lo;
        int hi = stack[sp].hi;

        while (lo < hi) {
            int i = lo;
            int j = hi;
            const int mid = lo + ((hi - lo) >> 1);
            const app_t pivot = g_apps[mid];

            while (i <= j) {
                while (strcmp(g_apps[i].base, pivot.base) < 0) i++;
                while (strcmp(g_apps[j].base, pivot.base) > 0) j--;
                if (i <= j) {
                    apps_swap_idx(i, j);
                    i++;
                    j--;
                }
            }

            const int left_lo = lo;
            const int left_hi = j;
            const int right_lo = i;
            const int right_hi = hi;

            const int left_len = left_hi - left_lo;
            const int right_len = right_hi - right_lo;

            if (left_len > right_len) {
                if (left_lo < left_hi) {
                    if (sp < (int)(sizeof(stack) / sizeof(stack[0]))) {
                        stack[sp].lo = left_lo;
                        stack[sp].hi = left_hi;
                        sp++;
                    }
                }
                lo = right_lo;
                hi = right_hi;
            } else {
                if (right_lo < right_hi) {
                    if (sp < (int)(sizeof(stack) / sizeof(stack[0]))) {
                        stack[sp].lo = right_lo;
                        stack[sp].hi = right_hi;
                        sp++;
                    }
                }
                lo = left_lo;
                hi = left_hi;
            }
        }
    }

    int w = 0;
    for (int r = 0; r < g_app_count; r++) {
        if (w == 0 || strcmp(g_apps[r].base, g_apps[w - 1].base) != 0) {
            if (w != r) g_apps[w] = g_apps[r];
            w++;
        }
    }
    g_app_count = w;
}

static void apps_add_filename(const char* name) {
    if (!name || !*name) return;
    if (!ends_with(name, ".exe")) return;

    char base[MAX_NAME];
    base[0] = '\0';
    {
        size_t n = strlen(name);
        if (n >= 4) n -= 4;
        if (n >= sizeof(base)) n = sizeof(base) - 1u;
        memcpy(base, name, n);
        base[n] = '\0';
    }
    if (!base[0]) return;
    apps_push_base(base);
}

static void apps_scan_dir(const char* path) {
    if (!path) return;

    int fd = open(path, 0);
    if (fd < 0) return;

    yfs_dirent_info_t dents[64];
    for (;;) {
        int n = getdents(fd, dents, (uint32_t)sizeof(dents));
        if (n <= 0) break;

        int cnt = n / (int)sizeof(yfs_dirent_info_t);
        for (int i = 0; i < cnt; i++) {
            yfs_dirent_info_t* d = &dents[i];
            if (d->inode == 0) continue;
            if (d->type == 2) continue;
            if (strcmp(d->name, ".") == 0 || strcmp(d->name, "..") == 0) continue;
            apps_add_filename(d->name);
        }
    }

    close(fd);
}

static void rebuild_filter(void) {
    g_filtered_count = 0;

    if (!g_filtered) {
        filtered_reserve(64);
        if (!g_filtered) return;
    }

    for (int i = 0; i < g_app_count; i++) {
        if (strstr(g_apps[i].base_lc, g_query_lc ? g_query_lc : "") != 0) {
            filtered_reserve(g_filtered_count + 1);
            if (!g_filtered || g_filtered_count >= g_filtered_cap) break;
            g_filtered[g_filtered_count++] = i;
        }
    }

    if (g_filtered_count <= 0) {
        g_selected = 0;
        g_scroll = 0;
        return;
    }

    if (g_selected < 0) g_selected = 0;
    if (g_selected >= g_filtered_count) g_selected = g_filtered_count - 1;

    if (g_scroll < 0) g_scroll = 0;
    if (g_scroll > g_selected) g_scroll = g_selected;
}

static int spawn_app(const char* base) {
    if (!base || !*base) return -1;

    char* argv[1];
    argv[0] = (char*)base;

    return spawn_process_resolved(base, 1, argv);
}

static void fill_rect(int x, int y, int w, int h, uint32_t color) {
    if (!canvas) return;
    for (int j = 0; j < h; j++) {
        int py = y + j;
        if ((unsigned)py >= (unsigned)WIN_H) continue;
        uint32_t* row = canvas + py * WIN_W;
        for (int i = 0; i < w; i++) {
            int px = x + i;
            if ((unsigned)px >= (unsigned)WIN_W) continue;
            row[px] = color;
        }
    }
}

static void draw_frame(int x, int y, int w, int h, uint32_t color) {
    fill_rect(x, y, w, 1, color);
    fill_rect(x, y + h - 1, w, 1, color);
    fill_rect(x, y, 1, h, color);
    fill_rect(x + w - 1, y, 1, h, color);
}

static void render(void) {
    fill_rect(0, 0, WIN_W, WIN_H, C_BG);

    const int panel_x = 12;
    const int panel_y = 12;
    const int panel_w = WIN_W - 24;
    const int panel_h = WIN_H - 24;

    fill_rect(panel_x, panel_y, panel_w, panel_h, C_PANEL_BG);
    draw_frame(panel_x, panel_y, panel_w, panel_h, C_BORDER);

    char header[96];
    (void)snprintf(header, sizeof(header), "Run: %s", g_query);
    draw_string(canvas, WIN_W, WIN_H, panel_x + PAD_X, panel_y + 14, header, C_TEXT);

    draw_string(canvas, WIN_W, WIN_H, panel_x + PAD_X, panel_y + 34, "Enter=run  Esc=close  Up/Down=select", C_MUTED);

    const int list_y = panel_y + 56;
    const int list_h = panel_h - (list_y - panel_y) - PAD_Y;
    const int rows = max_i(1, list_h / ROW_H);

    if (g_filtered_count == 0) {
        draw_string(canvas, WIN_W, WIN_H, panel_x + PAD_X, list_y + 14, "No matches", C_MUTED);
        return;
    }

    if (g_selected < g_scroll) g_scroll = g_selected;
    if (g_selected >= g_scroll + rows) g_scroll = g_selected - rows + 1;
    if (g_scroll < 0) g_scroll = 0;
    if (g_scroll > g_filtered_count - 1) g_scroll = g_filtered_count - 1;

    for (int i = 0; i < rows; i++) {
        const int idx = g_scroll + i;
        if (idx >= g_filtered_count) break;

        const int app_idx = g_filtered[idx];
        const char* name = g_apps[app_idx].base;

        const int row_y = list_y + i * ROW_H;
        const int text_y = row_y + 4;
        if (idx == g_selected) {
            fill_rect(panel_x + 2, row_y, panel_w - 4, ROW_H, C_SELECT_BG);
            draw_string(canvas, WIN_W, WIN_H, panel_x + PAD_X, text_y, name, 0xFFFFFF);
        } else {
            draw_string(canvas, WIN_W, WIN_H, panel_x + PAD_X, text_y, name, C_TEXT);
        }
    }
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
        (void)snprintf(new_name, sizeof(new_name), "launcher_%d_%d", getpid(), shm_gen);
        new_fd = shm_create_named(new_name, cap_bytes);
        if (new_fd >= 0) break;
    }
    if (new_fd < 0) return -1;

    uint32_t* new_canvas = (uint32_t*)mmap(new_fd, cap_bytes, MAP_SHARED);
    if (ptr_is_invalid(new_canvas)) {
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

__attribute__((force_align_arg_pointer)) int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    set_term_mode(0);

    dbg_write("launcher: start\n");

    g_apps = 0;
    g_app_count = 0;
    g_app_cap = 0;
    g_filtered = 0;
    g_filtered_count = 0;
    g_filtered_cap = 0;

    apps_scan_dir(".");
    apps_scan_dir("/bin");
    apps_scan_dir("/bin/usr");

    apps_sort_unique();

    g_query = 0;
    g_query_lc = 0;
    g_query_len = 0;
    g_query_cap = 0;
    query_reserve(32);
    if (!g_query || !g_query_lc) {
        dbg_write("launcher: query_reserve failed\n");
        return 1;
    }
    g_query[0] = '\0';
    g_query_lc[0] = '\0';
    g_selected = 0;
    g_scroll = 0;
    rebuild_filter();

    comp_conn_reset(&conn);
    if (comp_connect(&conn, "flux") != 0) {
        dbg_write("launcher: comp_connect failed\n");
        return 1;
    }
    if (comp_send_hello(&conn) != 0) {
        dbg_write("launcher: hello failed\n");
        comp_disconnect(&conn);
        return 1;
    }

    shm_name[0] = '\0';
    size_bytes = (uint32_t)WIN_W * (uint32_t)WIN_H * 4u;
    for (int i = 0; i < 8; i++) {
        (void)snprintf(shm_name, sizeof(shm_name), "launcher_%d_%d", getpid(), i);
        shm_fd = shm_create_named(shm_name, size_bytes);
        if (shm_fd >= 0) break;
    }
    if (shm_fd < 0) {
        dbg_write("launcher: shm_create_named failed\n");
        comp_disconnect(&conn);
        return 1;
    }

    canvas = (uint32_t*)mmap(shm_fd, size_bytes, MAP_SHARED);
    if (ptr_is_invalid(canvas)) {
        dbg_write("launcher: mmap failed\n");
        close(shm_fd);
        shm_fd = -1;
        shm_unlink_named(shm_name);
        shm_name[0] = '\0';
        comp_disconnect(&conn);
        return 1;
    }

    {
        uint16_t err = 0;
        int r = comp_send_attach_shm_name_sync(&conn, surface_id, shm_name, size_bytes, (uint32_t)WIN_W, (uint32_t)WIN_H, (uint32_t)WIN_W, 0u, 2000u, &err);
        if (r != 0) {
            char tmp[96];
            (void)snprintf(tmp, sizeof(tmp), "launcher: attach failed r=%d err=%u\n", r, (unsigned)err);
            dbg_write(tmp);
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

    const int pid = getpid();
    const int init_x = 120 + (pid % 5) * 20;
    const int init_y = 80 + (pid % 7) * 14;

    render();
    if (comp_send_commit(&conn, surface_id, init_x, init_y, 0u) != 0) {
        dbg_write("launcher: initial commit failed\n");
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
    while (running) {
        int need_update = 0;

        for (;;) {
            int rr = comp_try_recv(&conn, &hdr, payload, (uint32_t)sizeof(payload));
            if (rr < 0) {
                dbg_write("launcher: comp_try_recv failed\n");
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

            if (in.kind == COMP_IPC_INPUT_KEY) {
                if (in.key_state != 1u) continue;
                uint8_t kc = (uint8_t)in.keycode;

                if (kc == 0x1Bu) {
                    running = 0;
                    break;
                }

                if (kc == 0x0Au) {
                    if (g_filtered_count > 0) {
                        const int app_idx = g_filtered[g_selected];
                        const char* base = g_apps[app_idx].base;
                        (void)spawn_app(base);
                    }
                    running = 0;
                    break;
                }

                if (kc == 0x08u) {
                    if (g_query_len > 0) {
                        g_query_len--;
                        g_query[g_query_len] = '\0';
                        g_query_lc[g_query_len] = '\0';
                        rebuild_filter();
                        need_update = 1;
                    }
                    continue;
                }

                if (kc == 0x13u) {
                    if (g_filtered_count > 0 && g_selected > 0) {
                        g_selected--;
                        need_update = 1;
                    }
                    continue;
                }
                if (kc == 0x14u) {
                    if (g_filtered_count > 0 && g_selected + 1 < g_filtered_count) {
                        g_selected++;
                        need_update = 1;
                    }
                    continue;
                }

                if (kc >= 32u && kc <= 126u) {
                    query_reserve(g_query_len + 2);
                    if (g_query && g_query_lc && g_query_len + 1 < g_query_cap) {
                        g_query[g_query_len] = (char)kc;
                        g_query_lc[g_query_len] = lower_char((char)kc);
                        g_query_len++;
                        g_query[g_query_len] = '\0';
                        g_query_lc[g_query_len] = '\0';
                        rebuild_filter();
                        need_update = 1;
                    }
                    continue;
                }

                continue;
            }

            if (in.kind == COMP_IPC_INPUT_CLOSE) {
                dbg_write("launcher: close event\n");
                running = 0;
                break;
            }

            if (in.kind == COMP_IPC_INPUT_RESIZE) {
                const int32_t nw = in.x;
                const int32_t nh = in.y;
                if (nw > 0 && nh > 0 && (nw != WIN_W || nh != WIN_H)) {
                    if (ensure_surface((uint32_t)nw, (uint32_t)nh) == 0) {
                        WIN_W = (int)nw;
                        WIN_H = (int)nh;
                        need_update = 1;
                    }
                }
                continue;
            }
        }

        if (need_update && canvas) {
            render();
            if (comp_send_commit(&conn, surface_id, init_x, init_y, 0u) != 0) {
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

    if (g_apps) {
        free(g_apps);
        g_apps = 0;
    }
    if (g_filtered) {
        free(g_filtered);
        g_filtered = 0;
    }
    if (g_query) {
        free(g_query);
        g_query = 0;
    }
    if (g_query_lc) {
        free(g_query_lc);
        g_query_lc = 0;
    }
    return 0;
}
