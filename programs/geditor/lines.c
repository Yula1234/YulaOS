// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#include "lines.h"
#include "gapbuf.h"

static int lines_lower_bound(const LineIndex* li, int key) {
    if (!li || li->count <= 0) {
        return 0;
    }
    int lo = 0;
    int hi = li->count;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (li->starts[mid] < key) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

static int lines_upper_bound(const LineIndex* li, int key) {
    if (!li || li->count <= 0) {
        return 0;
    }
    int lo = 0;
    int hi = li->count;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (li->starts[mid] <= key) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

static void lines_recompute_c_block_from(LineIndex* li, const GapBuf* g, int from_line) {
    if (!li || !g) {
        return;
    }
    if (li->count <= 0) {
        return;
    }
    if (from_line < 0) {
        from_line = 0;
    }
    if (from_line >= li->count) {
        return;
    }
    if (!li->c_block) {
        return;
    }

    int in_block = li->c_block[from_line] ? 1 : 0;
    int text_len = gb_len(g);
    int line = from_line;

    for (int i = li->starts[from_line]; i < text_len && line + 1 < li->count; i++) {
        char c = gb_char_at(g, i);
        char n1 = (i + 1 < text_len) ? gb_char_at(g, i + 1) : 0;
        if (!in_block && c == '/' && n1 == '*') {
            in_block = 1;
            i++;
        } else if (in_block && c == '*' && n1 == '/') {
            in_block = 0;
            i++;
        }

        if (c == '\n') {
            line++;
            li->c_block[line] = (uint8_t)in_block;
        }
    }
}

void lines_init(LineIndex* li) {
    li->starts = 0;
    li->count = 0;
    li->cap = 0;
    li->c_block = 0;
    li->c_block_cap = 0;
}

void lines_destroy(LineIndex* li) {
    if (li->starts) {
        free(li->starts);
    }
    if (li->c_block) {
        free(li->c_block);
    }
    li->starts = 0;
    li->c_block = 0;
    li->count = 0;
    li->cap = 0;
    li->c_block_cap = 0;
}

int lines_ensure(LineIndex* li, int need) {
    if (need <= li->cap && need <= li->c_block_cap) {
        return 1;
    }

    int new_cap = li->cap ? li->cap : 64;
    while (new_cap < need) {
        new_cap *= 2;
    }

    int* ns = (int*)realloc(li->starts, (size_t)new_cap * sizeof(int));
    if (!ns) {
        return 0;
    }
    uint8_t* nb = (uint8_t*)realloc(li->c_block, (size_t)new_cap);
    if (!nb) {
        li->starts = ns;
        return 0;
    }

    li->starts = ns;
    li->c_block = nb;
    li->cap = new_cap;
    li->c_block_cap = new_cap;
    return 1;
}

void lines_rebuild(LineIndex* li, const GapBuf* g, int lang) {
    int len = gb_len(g);
    int approx = 2;
    for (int i = 0; i < len; i++) {
        if (gb_char_at(g, i) == '\n') {
            approx++;
        }
    }

    if (!lines_ensure(li, approx)) {
        li->count = 0;
        return;
    }

    int n = 0;
    li->starts[n] = 0;
    li->c_block[n] = 0;
    n++;

    int in_block = 0;
    for (int i = 0; i < len; i++) {
        char c = gb_char_at(g, i);
        if (lang == LANG_C) {
            char n1 = (i + 1 < len) ? gb_char_at(g, i + 1) : 0;
            if (!in_block && c == '/' && n1 == '*') {
                in_block = 1;
            } else if (in_block && c == '*' && n1 == '/') {
                in_block = 0;
            }
        }
        if (c == '\n') {
            if (n >= li->cap) {
                if (!lines_ensure(li, li->cap * 2)) {
                    break;
                }
            }
            li->starts[n] = i + 1;
            li->c_block[n] = (uint8_t)in_block;
            n++;
        }
    }
    li->count = n;
}

int lines_find_line(const LineIndex* li, int pos) {
    if (!li || li->count <= 0) {
        return 0;
    }
    if (pos <= 0) {
        return 0;
    }

    int lo = 0;
    int hi = li->count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int s = li->starts[mid];
        if (s <= pos) {
            if (mid + 1 >= li->count || li->starts[mid + 1] > pos) {
                return mid;
            }
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return 0;
}

int lines_apply_insert(LineIndex* li, const GapBuf* g, int pos, const char* s, int slen, int lang) {
    if (!li || !g) {
        return 0;
    }
    if (li->count <= 0) {
        lines_rebuild(li, g, lang);
        return 1;
    }

    int line = lines_find_line(li, pos);
    if (line < 0) {
        line = 0;
    }
    if (line >= li->count) {
        line = li->count - 1;
    }

    int nl = 0;
    for (int i = 0; i < slen; i++) {
        if (s[i] == '\n') {
            nl++;
        }
    }

    if (nl > 0) {
        if (!lines_ensure(li, li->count + nl)) {
            return 0;
        }

        int insert_at = line + 1;
        int tail = li->count - insert_at;
        if (tail > 0) {
            memmove(&li->starts[insert_at + nl], &li->starts[insert_at], (size_t)tail * sizeof(int));
            memmove(&li->c_block[insert_at + nl], &li->c_block[insert_at], (size_t)tail);
        }

        int j = 0;
        for (int i = 0; i < slen; i++) {
            if (s[i] == '\n') {
                li->starts[insert_at + j] = pos + i + 1;
                li->c_block[insert_at + j] = 0;
                j++;
            }
        }
        li->count += nl;

        for (int i = insert_at + nl; i < li->count; i++) {
            li->starts[i] += slen;
        }
    } else {
        for (int i = line + 1; i < li->count; i++) {
            li->starts[i] += slen;
        }
    }

    if (lang == LANG_C) {
        lines_recompute_c_block_from(li, g, line);
    }
    return 1;
}

int lines_apply_delete(LineIndex* li, const GapBuf* g, int start, int end, int lang) {
    if (!li || !g) {
        return 0;
    }
    if (li->count <= 0) {
        lines_rebuild(li, g, lang);
        return 1;
    }

    int delta = end - start;
    if (delta <= 0) {
        return 1;
    }

    int line = lines_find_line(li, start);
    if (line < 0) {
        line = 0;
    }
    if (line >= li->count) {
        line = li->count - 1;
    }

    int rm0 = lines_lower_bound(li, start + 1);
    int rm1 = lines_upper_bound(li, end);
    if (rm0 < 1) {
        rm0 = 1;
    }
    if (rm1 < rm0) {
        rm1 = rm0;
    }
    if (rm1 > li->count) {
        rm1 = li->count;
    }

    int rm = rm1 - rm0;
    if (rm > 0) {
        int tail = li->count - rm1;
        if (tail > 0) {
            memmove(&li->starts[rm0], &li->starts[rm1], (size_t)tail * sizeof(int));
            memmove(&li->c_block[rm0], &li->c_block[rm1], (size_t)tail);
        }
        li->count -= rm;
        if (line >= li->count) {
            line = li->count - 1;
        }
    }

    for (int i = rm0; i < li->count; i++) {
        li->starts[i] -= delta;
        if (li->starts[i] < 0) {
            li->starts[i] = 0;
        }
    }

    if (lang == LANG_C) {
        lines_recompute_c_block_from(li, g, line);
    }
    return 1;
}
