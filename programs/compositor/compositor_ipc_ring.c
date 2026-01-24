// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#include "compositor_internal.h"

uint32_t ipc_rx_count(const ipc_rx_ring_t* q) {
    return q->w - q->r;
}

void ipc_rx_reset(ipc_rx_ring_t* q) {
    q->r = 0;
    q->w = 0;
}

void ipc_rx_push(ipc_rx_ring_t* q, const uint8_t* src, uint32_t n) {
    if (!q || !src || n == 0) return;

    const uint32_t cap = (uint32_t)sizeof(q->buf);
    uint32_t count = ipc_rx_count(q);

    if (n > cap) {
        src += (n - cap);
        n = cap;
        q->r = 0;
        q->w = 0;
        count = 0;
    }

    if (count + n > cap) {
        uint32_t drop = (count + n) - cap;
        q->r += drop;
    }

    uint32_t mask = cap - 1u;
    uint32_t wi = q->w & mask;
    uint32_t first = cap - wi;
    if (first > n) first = n;
    memcpy(&q->buf[wi], src, first);
    if (n > first) {
        memcpy(&q->buf[0], src + first, n - first);
    }
    q->w += n;
}

void ipc_rx_peek(const ipc_rx_ring_t* q, uint32_t off, void* dst, uint32_t n) {
    uint8_t* out = (uint8_t*)dst;
    const uint32_t cap = (uint32_t)sizeof(q->buf);
    uint32_t mask = cap - 1u;
    uint32_t ri = (q->r + off) & mask;
    uint32_t first = cap - ri;
    if (first > n) first = n;
    memcpy(out, &q->buf[ri], first);
    if (n > first) {
        memcpy(out + first, &q->buf[0], n - first);
    }
}

void ipc_rx_drop(ipc_rx_ring_t* q, uint32_t n) {
    uint32_t count = ipc_rx_count(q);
    if (n > count) n = count;
    q->r += n;
}
