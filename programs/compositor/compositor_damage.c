// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#include "compositor_internal.h"

int rect_empty(const comp_rect_t* r) {
    return !r || r->x1 >= r->x2 || r->y1 >= r->y2;
}

comp_rect_t rect_make(int x, int y, int w, int h) {
    comp_rect_t r;
    r.x1 = x;
    r.y1 = y;
    r.x2 = x + w;
    r.y2 = y + h;
    return r;
}

comp_rect_t rect_intersect(comp_rect_t a, comp_rect_t b) {
    comp_rect_t r;
    r.x1 = (a.x1 > b.x1) ? a.x1 : b.x1;
    r.y1 = (a.y1 > b.y1) ? a.y1 : b.y1;
    r.x2 = (a.x2 < b.x2) ? a.x2 : b.x2;
    r.y2 = (a.y2 < b.y2) ? a.y2 : b.y2;
    return r;
}

comp_rect_t rect_union(comp_rect_t a, comp_rect_t b) {
    comp_rect_t r;
    r.x1 = (a.x1 < b.x1) ? a.x1 : b.x1;
    r.y1 = (a.y1 < b.y1) ? a.y1 : b.y1;
    r.x2 = (a.x2 > b.x2) ? a.x2 : b.x2;
    r.y2 = (a.y2 > b.y2) ? a.y2 : b.y2;
    return r;
}

int rect_overlaps_or_touches(comp_rect_t a, comp_rect_t b) {
    if (a.x2 < b.x1 - 1) return 0;
    if (b.x2 < a.x1 - 1) return 0;
    if (a.y2 < b.y1 - 1) return 0;
    if (b.y2 < a.y1 - 1) return 0;
    return 1;
}

comp_rect_t rect_clip_to_screen(comp_rect_t r, int w, int h) {
    comp_rect_t s = rect_make(0, 0, w, h);
    return rect_intersect(r, s);
}

void damage_reset(comp_damage_t* d) {
    if (!d) return;
    d->n = 0;
}

void damage_add(comp_damage_t* d, comp_rect_t r, int w, int h) {
    if (!d) return;

    r = rect_clip_to_screen(r, w, h);
    if (rect_empty(&r)) return;

    for (;;) {
        int merged = 0;
        for (int i = 0; i < d->n; i++) {
            if (rect_overlaps_or_touches(d->rects[i], r)) {
                r = rect_union(d->rects[i], r);
                d->rects[i] = d->rects[d->n - 1];
                d->n--;
                merged = 1;
                break;
            }
        }
        if (!merged) break;
    }

    if (d->n < COMP_MAX_DAMAGE_RECTS) {
        d->rects[d->n++] = r;
    } else {
        comp_rect_t u = d->rects[0];
        for (int i = 1; i < d->n; i++) u = rect_union(u, d->rects[i]);
        u = rect_union(u, r);
        d->rects[0] = u;
        d->n = 1;
    }
}
