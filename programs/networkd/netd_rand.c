// SPDX-License-Identifier: GPL-2.0

#include "netd_rand.h"

#include <string.h>

#include <yula.h>

#include "netd_sha256.h"

static uint64_t netd_rdtsc(void) {
    uint32_t lo = 0;
    uint32_t hi = 0;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

void netd_rand_init(netd_rand_t* r) {
    if (!r) {
        return;
    }

    memset(r, 0, sizeof(*r));
}

void netd_rand_stir(netd_rand_t* r, const void* data, uint32_t len) {
    if (!r || (!data && len != 0)) {
        return;
    }

    netd_sha256_t s;
    netd_sha256_init(&s);
    netd_sha256_update(&s, r->state, (uint32_t)sizeof(r->state));
    if (len > 0) {
        netd_sha256_update(&s, data, len);
    }
    uint32_t c = r->ctr;
    netd_sha256_update(&s, &c, (uint32_t)sizeof(c));
    netd_sha256_final(&s, r->state);

    r->ctr += 1u;
    r->seeded = 1;
}

static void netd_rand_seed_if_needed(netd_rand_t* r) {
    if (!r || r->seeded) {
        return;
    }

    uint8_t seed[64];
    memset(seed, 0, sizeof(seed));

    uint32_t now = uptime_ms();
    memcpy(seed + 0, &now, (uint32_t)sizeof(now));

    uint64_t t0 = netd_rdtsc();
    uint64_t t1 = netd_rdtsc();
    uint64_t t2 = netd_rdtsc();
    uint64_t t3 = netd_rdtsc();

    memcpy(seed + 8, &t0, (uint32_t)sizeof(t0));
    memcpy(seed + 16, &t1, (uint32_t)sizeof(t1));
    memcpy(seed + 24, &t2, (uint32_t)sizeof(t2));
    memcpy(seed + 32, &t3, (uint32_t)sizeof(t3));

    netd_rand_stir(r, seed, (uint32_t)sizeof(seed));
    memset(seed, 0, sizeof(seed));
}

void netd_rand_bytes(netd_rand_t* r, void* out, uint32_t len) {
    if (!r || (!out && len != 0)) {
        return;
    }

    netd_rand_seed_if_needed(r);

    uint8_t* dst = (uint8_t*)out;
    uint32_t off = 0;
    while (off < len) {
        uint8_t block[32];
        netd_sha256_t s;
        netd_sha256_init(&s);
        netd_sha256_update(&s, r->state, (uint32_t)sizeof(r->state));
        uint32_t ctr = r->ctr;
        netd_sha256_update(&s, &ctr, (uint32_t)sizeof(ctr));
        uint64_t t = netd_rdtsc();
        netd_sha256_update(&s, &t, (uint32_t)sizeof(t));
        netd_sha256_final(&s, block);

        uint32_t take = len - off;
        if (take > (uint32_t)sizeof(block)) {
            take = (uint32_t)sizeof(block);
        }
        memcpy(dst + off, block, take);
        off += take;

        netd_rand_stir(r, block, (uint32_t)sizeof(block));
        memset(block, 0, sizeof(block));
    }
}

