// SPDX-License-Identifier: GPL-2.0

#include <yula.h>

#include <net_ipc.h>

static int net_write_all(int fd, const void* buf, uint32_t size) {
    const uint8_t* p = (const uint8_t*)buf;
    uint32_t off = 0;
    while (off < size) {
        uint32_t left = size - off;
        int r = write(fd, p + off, left);
        if (r <= 0) {
            return -1;
        }
        off += (uint32_t)r;
    }
    return 0;
}

uint32_t net_ipc_rx_count(const net_ipc_rx_t* rx) {
    if (!rx) return 0;
    return rx->w - rx->r;
}

void net_ipc_rx_reset(net_ipc_rx_t* rx) {
    if (!rx) return;
    rx->r = 0;
    rx->w = 0;
}

void net_ipc_rx_push(net_ipc_rx_t* rx, const uint8_t* src, uint32_t n) {
    if (!rx || !src || n == 0) return;

    const uint32_t cap = (uint32_t)sizeof(rx->buf);
    uint32_t count = net_ipc_rx_count(rx);

    if (n > cap) {
        src += (n - cap);
        n = cap;
        rx->r = 0;
        rx->w = 0;
        count = 0;
    }

    if (count + n > cap) {
        uint32_t drop = (count + n) - cap;
        rx->r += drop;
    }

    uint32_t mask = cap - 1u;
    uint32_t wi = rx->w & mask;
    uint32_t first = cap - wi;
    if (first > n) first = n;
    memcpy(&rx->buf[wi], src, first);
    if (n > first) {
        memcpy(&rx->buf[0], src + first, n - first);
    }
    rx->w += n;
}

void net_ipc_rx_peek(const net_ipc_rx_t* rx, uint32_t off, void* dst, uint32_t n) {
    if (!rx || !dst || n == 0) return;

    uint8_t* out = (uint8_t*)dst;
    const uint32_t cap = (uint32_t)sizeof(rx->buf);
    uint32_t mask = cap - 1u;
    uint32_t ri = (rx->r + off) & mask;
    uint32_t first = cap - ri;
    if (first > n) first = n;
    memcpy(out, &rx->buf[ri], first);
    if (n > first) {
        memcpy(out + first, &rx->buf[0], n - first);
    }
}

void net_ipc_rx_drop(net_ipc_rx_t* rx, uint32_t n) {
    if (!rx) return;
    uint32_t count = net_ipc_rx_count(rx);
    if (n > count) n = count;
    rx->r += n;
}

int net_ipc_send(int fd, uint16_t type, uint32_t seq, const void* payload, uint32_t len) {
    if (len > NET_IPC_MAX_PAYLOAD) {
        return -1;
    }

    net_ipc_hdr_t hdr;
    hdr.magic = NET_IPC_MAGIC;
    hdr.version = NET_IPC_VERSION;
    hdr.type = type;
    hdr.len = len;
    hdr.seq = seq;

    if (net_write_all(fd, &hdr, (uint32_t)sizeof(hdr)) != 0) {
        return -1;
    }
    if (len > 0) {
        if (net_write_all(fd, payload, len) != 0) {
            return -1;
        }
    }
    return 0;
}

int net_ipc_try_recv(net_ipc_rx_t* rx, int fd, net_ipc_hdr_t* out_hdr, uint8_t* out_payload, uint32_t cap) {
    if (!rx || fd < 0 || !out_hdr) {
        return -1;
    }

    for (;;) {
        uint8_t tmp[256];
        int rn = pipe_try_read(fd, tmp, (uint32_t)sizeof(tmp));
        if (rn < 0) {
            return -1;
        }
        if (rn == 0) break;
        net_ipc_rx_push(rx, tmp, (uint32_t)rn);
    }

    for (;;) {
        uint32_t avail = net_ipc_rx_count(rx);
        if (avail < 4) return 0;

        uint32_t magic = 0;
        net_ipc_rx_peek(rx, 0, &magic, 4);
        if (magic != NET_IPC_MAGIC) {
            net_ipc_rx_drop(rx, 1);
            continue;
        }

        if (avail < (uint32_t)sizeof(net_ipc_hdr_t)) return 0;

        net_ipc_hdr_t hdr;
        net_ipc_rx_peek(rx, 0, &hdr, (uint32_t)sizeof(hdr));
        if (hdr.version != NET_IPC_VERSION || hdr.len > NET_IPC_MAX_PAYLOAD) {
            net_ipc_rx_drop(rx, 1);
            continue;
        }

        uint32_t frame_len = (uint32_t)sizeof(net_ipc_hdr_t) + hdr.len;
        if (avail < frame_len) return 0;

        net_ipc_rx_drop(rx, (uint32_t)sizeof(net_ipc_hdr_t));
        if (hdr.len > 0 && out_payload) {
            uint32_t copy_len = hdr.len;
            if (copy_len > cap) copy_len = cap;
            net_ipc_rx_peek(rx, 0, out_payload, copy_len);
            net_ipc_rx_drop(rx, hdr.len);
        } else {
            net_ipc_rx_drop(rx, hdr.len);
        }

        *out_hdr = hdr;
        return 1;
    }
}
