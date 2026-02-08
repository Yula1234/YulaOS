#include "gapbuf.h"

static int gb_gap_size(const GapBuf* g) {
    return g->gap_end - g->gap_start;
}

int gb_len(const GapBuf* g) {
    return g->cap - (g->gap_end - g->gap_start);
}

void gb_init(GapBuf* g, int initial_cap) {
    if (initial_cap < 64) {
        initial_cap = 64;
    }
    g->buf = (char*)malloc((size_t)initial_cap);
    g->cap = g->buf ? initial_cap : 0;
    g->gap_start = 0;
    g->gap_end = g->cap;
}

void gb_destroy(GapBuf* g) {
    if (g->buf) {
        free(g->buf);
    }
    g->buf = 0;
    g->cap = 0;
    g->gap_start = 0;
    g->gap_end = 0;
}

char gb_char_at(const GapBuf* g, int pos) {
    int len = gb_len(g);
    if (pos < 0 || pos >= len) {
        return 0;
    }
    if (pos < g->gap_start) {
        return g->buf[pos];
    }
    return g->buf[pos + gb_gap_size(g)];
}

static void gb_move_gap(GapBuf* g, int pos) {
    int len = gb_len(g);
    if (pos < 0) {
        pos = 0;
    }
    if (pos > len) {
        pos = len;
    }

    if (pos < g->gap_start) {
        int move = g->gap_start - pos;
        memmove(g->buf + g->gap_end - move, g->buf + pos, (size_t)move);
        g->gap_start -= move;
        g->gap_end -= move;
    } else if (pos > g->gap_start) {
        int move = pos - g->gap_start;
        memmove(g->buf + g->gap_start, g->buf + g->gap_end, (size_t)move);
        g->gap_start += move;
        g->gap_end += move;
    }
}

static int gb_ensure_gap(GapBuf* g, int need) {
    if (need <= gb_gap_size(g)) {
        return 1;
    }

    int len = gb_len(g);
    int new_cap = g->cap;
    while (new_cap - len < need) {
        if (new_cap < 1024) {
            new_cap *= 2;
        } else {
            new_cap += new_cap / 2;
        }
        if (new_cap < g->cap) {
            return 0;
        }
    }

    char* nb = (char*)malloc((size_t)new_cap);
    if (!nb) {
        return 0;
    }

    int before = g->gap_start;
    int after = g->cap - g->gap_end;
    if (before) {
        memcpy(nb, g->buf, (size_t)before);
    }
    if (after) {
        memcpy(nb + (new_cap - after), g->buf + g->gap_end, (size_t)after);
    }
    free(g->buf);
    g->buf = nb;
    g->cap = new_cap;
    g->gap_start = before;
    g->gap_end = new_cap - after;
    return 1;
}

int gb_insert_at(GapBuf* g, int pos, const char* s, int slen) {
    if (slen <= 0) {
        return 1;
    }
    gb_move_gap(g, pos);
    if (!gb_ensure_gap(g, slen)) {
        return 0;
    }
    memcpy(g->buf + g->gap_start, s, (size_t)slen);
    g->gap_start += slen;
    return 1;
}

int gb_delete_range(GapBuf* g, int start, int end) {
    int len = gb_len(g);
    if (start < 0) {
        start = 0;
    }
    if (end > len) {
        end = len;
    }
    if (start >= end) {
        return 1;
    }
    gb_move_gap(g, start);
    g->gap_end += (end - start);
    if (g->gap_end > g->cap) {
        g->gap_end = g->cap;
    }
    return 1;
}

char* gb_copy_range(const GapBuf* g, int start, int end) {
    int len = gb_len(g);
    if (start < 0) {
        start = 0;
    }
    if (end > len) {
        end = len;
    }
    if (start >= end) {
        char* z = (char*)malloc(1);
        if (z) {
            z[0] = 0;
        }
        return z;
    }

    int n = end - start;
    char* out = (char*)malloc((size_t)n + 1);
    if (!out) {
        return 0;
    }
    for (int i = 0; i < n; i++) {
        out[i] = gb_char_at(g, start + i);
    }
    out[n] = 0;
    return out;
}

static int gb_match_at(const GapBuf* g, int pos, const char* needle, int nlen) {
    if (nlen <= 0) {
        return 1;
    }
    for (int i = 0; i < nlen; i++) {
        if (gb_char_at(g, pos + i) != needle[i]) {
            return 0;
        }
    }
    return 1;
}

int gb_find_forward(const GapBuf* g, int start, const char* needle, int nlen) {
    int len = gb_len(g);
    if (nlen <= 0 || !needle) {
        return -1;
    }
    if (start < 0) {
        start = 0;
    }
    if (start > len) {
        start = len;
    }

    for (int i = start; i + nlen <= len; i++) {
        if (gb_match_at(g, i, needle, nlen)) {
            return i;
        }
    }
    return -1;
}
