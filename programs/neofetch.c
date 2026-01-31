#include <yula.h>

#define RESET "\x1b[0m"
#define BOLD  "\x1b[1m"

#define KEY_COL        "\x1b[1;34m"
#define HOST_USER_COL  "\x1b[1;32m"
#define HOST_HOST_COL  "\x1b[1;36m"

#define LOGO_L "\x1b[94m"
#define LOGO_R "\x1b[96m"
#define LOGO_S "\x1b[90m"
#define LOGO_H "\x1b[97m"

#define LOGO_LB  "\x1b[44m"
#define LOGO_RB  "\x1b[46m"
#define LOGO_LBH "\x1b[104m"
#define LOGO_RBH "\x1b[106m"
#define LOGO_SB  "\x1b[100m"

enum {
    LOGO_PX_NONE = 0,
    LOGO_PX_L = 1,
    LOGO_PX_R = 2,
    LOGO_PX_LH = 3,
    LOGO_PX_RH = 4,
    LOGO_PX_S = 5,
};

static const char* logo_px_bg(uint8_t px) {
    switch (px) {
        case LOGO_PX_L:  return LOGO_LB;
        case LOGO_PX_R:  return LOGO_RB;
        case LOGO_PX_LH: return LOGO_LBH;
        case LOGO_PX_RH: return LOGO_RBH;
        case LOGO_PX_S:  return LOGO_SB;
        default:         return RESET;
    }
}

static uint8_t logo_px_from_ch(char ch) {
    switch (ch) {
        case 'B': return LOGO_PX_L;
        case 'C': return LOGO_PX_R;
        case 'b': return LOGO_PX_LH;
        case 'c': return LOGO_PX_RH;
        case 's': return LOGO_PX_S;
        default:  return LOGO_PX_NONE;
    }
}

static void sb_append(char* out, size_t cap, size_t* pos, const char* s) {
    if (!out || cap == 0 || !pos || !s) return;
    if (*pos >= cap) return;
    while (*s && (*pos + 1u) < cap) {
        out[*pos] = *s;
        (*pos)++;
        s++;
    }
    out[*pos] = '\0';
}

static void build_logo_line(char* out, size_t cap, const char* row_mask, int cols) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!row_mask || cols <= 0) return;

    size_t pos = 0;
    sb_append(out, cap, &pos, " ");

    int last = cols - 1;
    while (last >= 0 && logo_px_from_ch(row_mask[last]) == LOGO_PX_NONE) last--;
    if (last < 0) {
        sb_append(out, cap, &pos, RESET);
        return;
    }

    uint8_t cur = 0xFFu;
    for (int x = 0; x <= last; x++) {
        const uint8_t px = logo_px_from_ch(row_mask[x]);
        if (px != cur) {
            sb_append(out, cap, &pos, logo_px_bg(px));
            cur = px;
        }
        sb_append(out, cap, &pos, " ");
    }
    sb_append(out, cap, &pos, RESET);
}

static void logo_mask_clear(char* mask, int rows, int cols, int stride) {
    if (!mask || rows <= 0 || cols <= 0 || stride <= cols) return;
    for (int y = 0; y < rows; y++) {
        char* row = mask + y * stride;
        memset(row, '.', (size_t)cols);
        row[cols] = '\0';
    }
}

static void logo_mask_set(char* mask, int rows, int cols, int stride, int x, int y, char ch) {
    if (!mask) return;
    if (x < 0 || x >= cols) return;
    if (y < 0 || y >= rows) return;
    mask[y * stride + x] = ch;
}

static void logo_stamp_disc(char* mask, int rows, int cols, int stride, int cx, int cy, int r, char ch) {
    if (!mask || rows <= 0 || cols <= 0 || stride <= cols) return;
    if (r <= 0) {
        logo_mask_set(mask, rows, cols, stride, cx, cy, ch);
        return;
    }

    const int rr = r * r;
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            if ((dx * dx + dy * dy) <= rr) {
                logo_mask_set(mask, rows, cols, stride, cx + dx, cy + dy, ch);
            }
        }
    }
}

static void logo_draw_line_disc(char* mask, int rows, int cols, int stride, int x0, int y0, int x1, int y1, int r, char ch) {
    int dx = (x0 < x1) ? (x1 - x0) : (x0 - x1);
    int sx = (x0 < x1) ? 1 : -1;
    int dy = (y0 < y1) ? (y1 - y0) : (y0 - y1);
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;

    for (;;) {
        logo_stamp_disc(mask, rows, cols, stride, x0, y0, r, ch);
        if (x0 == x1 && y0 == y1) break;
        int e2 = err * 2;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
}

static void logo_draw_polyline_disc(char* mask, int rows, int cols, int stride, const int (*pts)[2], int n, int r, char ch) {
    if (!pts || n < 2) return;
    for (int i = 0; i < (n - 1); i++) {
        logo_draw_line_disc(mask, rows, cols, stride, pts[i][0], pts[i][1], pts[i + 1][0], pts[i + 1][1], r, ch);
    }
}

static void logo_fill_loop_pts(int pts[9][2], int cx, int cy, int rx, int ry, int tilt_dir) {
    if (!pts) return;
    if (tilt_dir < 0) tilt_dir = -1;
    else tilt_dir = 1;

    int tilt = rx / 3;
    if (tilt < 1) tilt = 1;

    const int top_x = cx + tilt_dir * tilt;
    const int bot_x = cx - tilt_dir * tilt;

    pts[0][0] = bot_x;
    pts[0][1] = cy + ry;

    pts[1][0] = cx - rx;
    pts[1][1] = cy + (ry / 2);

    pts[2][0] = cx - rx - (tilt_dir * (tilt / 2));
    pts[2][1] = cy;

    pts[3][0] = cx - (rx / 2);
    pts[3][1] = cy - ry + 1;

    pts[4][0] = top_x;
    pts[4][1] = cy - ry;

    pts[5][0] = cx + rx;
    pts[5][1] = cy - (ry / 2);

    pts[6][0] = cx + rx + (tilt_dir * (tilt / 2));
    pts[6][1] = cy;

    pts[7][0] = cx + (rx / 2);
    pts[7][1] = cy + ry - 1;

    pts[8][0] = pts[0][0];
    pts[8][1] = pts[0][1];
}

static void logo_draw_ribbon_loop(char* mask, int rows, int cols, int stride, const int (*pts)[2], int n, char base, char hi) {
    const int r_outer = 2;
    const int r_inner = 1;

    logo_draw_polyline_disc(mask, rows, cols, stride, pts, n, r_outer, base);
    logo_draw_polyline_disc(mask, rows, cols, stride, pts, n, r_inner, '.');

    for (int i = 1; i <= 5 && i < (n - 1); i++) {
        logo_draw_line_disc(mask, rows, cols, stride, pts[i][0], pts[i][1], pts[i + 1][0], pts[i + 1][1], 1, hi);
    }
}

static void logo_apply_drop_shadow(char* dst, int dst_stride, const char* src, int src_stride, int rows, int cols, int ox, int oy) {
    if (!dst || !src) return;
    if (dst_stride <= cols || src_stride <= cols) return;
    for (int y = 0; y < rows; y++) {
        for (int x = 0; x < cols; x++) {
            const char ch = src[y * src_stride + x];
            if (ch == '.') continue;
            const int sx = x + ox;
            const int sy = y + oy;
            if (sx < 0 || sx >= cols) continue;
            if (sy < 0 || sy >= rows) continue;
            if (dst[sy * dst_stride + sx] == '.') {
                dst[sy * dst_stride + sx] = 's';
            }
        }
    }
}

static void logo_overlay(char* dst, int dst_stride, const char* src, int src_stride, int rows, int cols) {
    if (!dst || !src) return;
    if (dst_stride <= cols || src_stride <= cols) return;
    for (int y = 0; y < rows; y++) {
        for (int x = 0; x < cols; x++) {
            const char ch = src[y * src_stride + x];
            if (ch == '.') continue;
            dst[y * dst_stride + x] = ch;
        }
    }
}

static void logo_draw_ribbon_y(char* mask, int rows, int cols, int stride) {
    if (!mask || rows <= 0 || cols <= 0 || stride <= cols) return;

    char base[24][64];
    if (rows > 24 || cols > 63) {
        logo_mask_clear(mask, rows, cols, stride);
        return;
    }
    logo_mask_clear((char*)base, rows, cols, 64);

    static const char* const map[] = {
        "....bBBB..................CCCc....",
        ".....bBBB................CCCc.....",
        "......bBBB..............CCCc......",
        ".......bBBB............CCCc.......",
        "........bBBB..........CCCc........",
        ".........bBBB........CCCc.........",
        "..........bBBBBBBCCCCCCc..........",
        "..............bBBCCc..............",
        "..............bBBCCc..............",
        "..............bBBCCc..............",
        "..............bBBCCc..............",
        "..............bBBCCc..............",
        "..............bBBCCc..............",
        "..............bBBCCc..............",
    };

    const int map_rows = (int)(sizeof(map) / sizeof(map[0]));
    for (int y = 0; y < rows && y < map_rows; y++) {
        const char* row = map[y];
        const int n = (int)strlen(row);
        const int w = (n < cols) ? n : cols;
        for (int x = 0; x < w; x++) {
            const char ch = row[x];
            if (ch != '.') base[y][x] = ch;
        }
    }

    logo_mask_clear(mask, rows, cols, stride);
    logo_apply_drop_shadow(mask, stride, (const char*)base, 64, rows, cols, 1, 1);
    logo_overlay(mask, stride, (const char*)base, 64, rows, cols);
}

static void fmt_uptime(char* out, size_t cap, uint32_t ms) {
    if (!out || cap == 0) return;

    uint32_t sec = ms / 1000u;
    uint32_t days = sec / 86400u;
    sec %= 86400u;
    uint32_t hours = sec / 3600u;
    sec %= 3600u;
    uint32_t mins = sec / 60u;
    sec %= 60u;

    if (days) (void)snprintf(out, cap, "%u day%s, %u:%02u:%02u", days, (days == 1u) ? "" : "s", hours, mins, sec);
    else (void)snprintf(out, cap, "%u:%02u:%02u", hours, mins, sec);
}

static void fmt_mem(char* out, size_t cap, uint32_t used_kib, uint32_t free_kib) {
    if (!out || cap == 0) return;
    const uint32_t total_kib = used_kib + free_kib;
    if (!total_kib) {
        (void)snprintf(out, cap, "unknown");
        return;
    }

    const uint32_t used_mib = used_kib / 1024u;
    const uint32_t total_mib = total_kib / 1024u;

    uint32_t pct = 0;
    if (used_kib >= total_kib) {
        pct = 100u;
    } else if (used_kib <= (UINT32_MAX / 100u)) {
        pct = (used_kib * 100u) / total_kib;
    } else {
        pct = 100u;
    }

    (void)snprintf(out, cap, "%u MiB / %u MiB (%u%%)", used_mib, total_mib, pct);
}

static uint32_t count_procs(void) {
    uint32_t cap = 64;
    yos_proc_info_t* list = 0;

    for (int iter = 0; iter < 8; iter++) {
        const uint32_t bytes = cap * (uint32_t)sizeof(*list);
        yos_proc_info_t* next = (yos_proc_info_t*)realloc(list, bytes);
        if (!next) {
            if (list) free(list);
            return 0;
        }
        list = next;

        int n = proc_list(list, cap);
        if (n < 0) {
            free(list);
            return 0;
        }
        if ((uint32_t)n < cap) {
            free(list);
            return (uint32_t)n;
        }

        uint32_t next_cap = cap * 2u;
        if (next_cap <= cap) break;
        cap = next_cap;
    }

    if (list) free(list);
    return 0;
}

static const char* arch_name(void) {
#if defined(__i386__)
    return "i386";
#elif defined(__x86_64__)
    return "x86_64";
#else
    return "unknown";
#endif
}

static void fmt_kv(char* out, size_t cap, const char* key, const char* val) {
    if (!out || cap == 0) return;
    if (!key) key = "";
    if (!val) val = "";
    (void)snprintf(out, cap, "%s%-8s%s %s", KEY_COL, key, RESET, val);
}

static void fmt_colorbar(char* out, size_t cap) {
    if (!out || cap == 0) return;
    (void)snprintf(out, cap,
                   "\x1b[40m   \x1b[41m   \x1b[42m   \x1b[43m   \x1b[44m   \x1b[45m   \x1b[46m   \x1b[47m   "
                   "\x1b[100m   \x1b[101m   \x1b[102m   \x1b[103m   \x1b[104m   \x1b[105m   \x1b[106m   \x1b[107m   "
                   RESET);
}

static int ansi_visible_len(const char* s) {
    if (!s) return 0;
    int n = 0;
    const unsigned char* p = (const unsigned char*)s;
    while (*p) {
        if (*p == 0x1Bu && p[1] == (unsigned char)'[') {
            p += 2;
            while (*p && ((*p >= (unsigned char)'0' && *p <= (unsigned char)'9') || *p == (unsigned char)';')) {
                p++;
            }
            if (*p) p++;
            continue;
        }
        n++;
        p++;
    }
    return n;
}

static void print_logo_cell(const char* text, int width) {
    if (!text) text = "";

    const int n = ansi_visible_len(text);
    if (n) {
        printf("%s%s", text, RESET);
    } else {
        printf("%s", RESET);
    }

    int pad = width - n;
    if (pad < 0) pad = 0;
    while (pad--) putchar(' ');
}

static int logo_max_width(const char* const* logo, int logo_n) {
    if (!logo || logo_n <= 0) return 0;
    int w = 0;
    for (int i = 0; i < logo_n; i++) {
        const int n = ansi_visible_len(logo[i]);
        if (n > w) w = n;
    }
    return w;
}

static void print_logo_info(const char* const* logo, int logo_n, int logo_w, const char* const* info, int info_n) {
    if (!logo || logo_n <= 0) return;
    if (!info || info_n < 0) return;

    int rows = (logo_n > info_n) ? logo_n : info_n;
    for (int i = 0; i < rows; i++) {
        if (i < logo_n) {
            print_logo_cell(logo[i], logo_w);
        } else {
            print_logo_cell("", logo_w);
        }
        printf("  %s\n", (i < info_n) ? info[i] : "");
    }
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    uint32_t used_kib = 0;
    uint32_t free_kib = 0;
    (void)syscall(12, (int)(uintptr_t)&used_kib, (int)(uintptr_t)&free_kib, 0);

    char up[64];
    fmt_uptime(up, sizeof(up), uptime_ms());

    char mem[64];
    fmt_mem(mem, sizeof(mem), used_kib, free_kib);

    uint32_t procs = count_procs();

    enum { LOGO_ROWS = 14, LOGO_COLS = 34 };
    char logo_mask[LOGO_ROWS][LOGO_COLS + 1];
    logo_draw_ribbon_y((char*)logo_mask, LOGO_ROWS, LOGO_COLS, LOGO_COLS + 1);

    char logo_lines[LOGO_ROWS][512];
    const char* logo[LOGO_ROWS];
    for (int i = 0; i < LOGO_ROWS; i++) {
        build_logo_line(logo_lines[i], sizeof(logo_lines[i]), logo_mask[i], LOGO_COLS);
        logo[i] = logo_lines[i];
    }
    const int logo_n = LOGO_ROWS;
    const int logo_w = logo_max_width(logo, logo_n);

    const char host_plain[] = "user@yulaos";
    char host_line[64];
    (void)snprintf(host_line, sizeof(host_line), "%suser%s@%syulaos%s", HOST_USER_COL, RESET, HOST_HOST_COL, RESET);

    char sep[32];
    size_t sep_n = strlen(host_plain);
    if (sep_n >= sizeof(sep)) sep_n = sizeof(sep) - 1u;
    memset(sep, '-', sep_n);
    sep[sep_n] = '\0';

    char os_line[128];
    char up_line[128];
    char mem_line[128];
    char proc_line[128];

    char os_val[64];
    (void)snprintf(os_val, sizeof(os_val), "YulaOS (%s)", arch_name());
    fmt_kv(os_line, sizeof(os_line), "OS", os_val);
    fmt_kv(up_line, sizeof(up_line), "Uptime", up);
    fmt_kv(mem_line, sizeof(mem_line), "Memory", mem);

    char proc_val[32];
    (void)snprintf(proc_val, sizeof(proc_val), "%u", procs);
    fmt_kv(proc_line, sizeof(proc_line), "Procs", proc_val);

    char bar[160];
    fmt_colorbar(bar, sizeof(bar));

    const char* info[] = {
        host_line,
        sep,
        os_line,
        up_line,
        mem_line,
        proc_line,
        bar,
    };
    const int info_n = (int)(sizeof(info) / sizeof(info[0]));

    putchar('\n');

    print_logo_info(logo, logo_n, logo_w, info, info_n);

    return 0;
}
