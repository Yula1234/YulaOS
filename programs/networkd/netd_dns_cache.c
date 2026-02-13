// SPDX-License-Identifier: GPL-2.0

#include "netd_dns_cache.h"

#include <yula.h>

#include "netd_config.h"

static uint32_t netd_dns_cache_ensure_capacity(netd_dns_cache_t* cache, uint32_t min_cap) {
    if (!cache) {
        return 0;
    }

    if (cache->capacity >= min_cap) {
        return 1;
    }

    uint32_t new_cap = cache->capacity == 0 ? NETD_DNS_CACHE_SIZE : cache->capacity;
    while (new_cap < min_cap) {
        new_cap *= 2;
    }

    if (new_cap > NETD_DNS_CACHE_SIZE * 4) {
        new_cap = NETD_DNS_CACHE_SIZE * 4;
    }

    if (new_cap < min_cap) {
        return 0;
    }

    netd_dns_cache_entry_t* new_entries = (netd_dns_cache_entry_t*)realloc(
        cache->entries,
        new_cap * sizeof(netd_dns_cache_entry_t)
    );

    if (!new_entries) {
        return 0;
    }

    if (new_cap > cache->capacity) {
        memset(
            new_entries + cache->capacity,
            0,
            (new_cap - cache->capacity) * sizeof(netd_dns_cache_entry_t)
        );
    }

    cache->entries = new_entries;
    cache->capacity = new_cap;
    return 1;
}

void netd_dns_cache_init(netd_dns_cache_t* cache) {
    if (!cache) {
        return;
    }

    memset(cache, 0, sizeof(*cache));

    if (!netd_dns_cache_ensure_capacity(cache, NETD_DNS_CACHE_SIZE)) {
        cache->entries = NULL;
        cache->capacity = 0;
    }
}

void netd_dns_cache_cleanup(netd_dns_cache_t* cache) {
    if (!cache) {
        return;
    }

    if (cache->entries) {
        free(cache->entries);
        cache->entries = NULL;
    }

    cache->count = 0;
    cache->capacity = 0;
    cache->hits = 0;
    cache->misses = 0;
}

uint32_t netd_dns_cache_hash(const char* name) {
    if (!name) {
        return 0;
    }

    uint32_t hash = 5381u;
    const uint8_t* p = (const uint8_t*)name;

    while (*p) {
        uint8_t c = *p;
        if (c >= 'A' && c <= 'Z') {
            c = c + 32;
        }
        hash = ((hash << 5) + hash) + c;
        p++;
    }

    return hash;
}

static int netd_dns_name_equal(const char* a, const char* b) {
    if (!a || !b) {
        return 0;
    }

    while (*a && *b) {
        char ca = *a;
        char cb = *b;

        if (ca >= 'A' && ca <= 'Z') {
            ca = ca + 32;
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = cb + 32;
        }

        if (ca != cb) {
            return 0;
        }

        a++;
        b++;
    }

    return *a == *b;
}

void netd_dns_cache_expire_old(netd_dns_cache_t* cache) {
    if (!cache || !cache->entries) {
        return;
    }

    uint32_t now = uptime_ms();
    uint32_t i = 0;

    while (i < cache->count) {
        netd_dns_cache_entry_t* e = &cache->entries[i];

        if (e->ttl_ms > 0) {
            uint32_t age = now - e->timestamp_ms;
            if (age >= e->ttl_ms) {
                if (i < cache->count - 1) {
                    cache->entries[i] = cache->entries[cache->count - 1];
                }
                cache->count--;
                continue;
            }
        }

        i++;
    }
}

int netd_dns_cache_lookup(netd_dns_cache_t* cache, const char* name, uint32_t* out_addr) {
    if (!cache || !cache->entries || !name || !out_addr) {
        return 0;
    }

    netd_dns_cache_expire_old(cache);

    for (uint32_t i = 0; i < cache->count; i++) {
        netd_dns_cache_entry_t* e = &cache->entries[i];

        if (netd_dns_name_equal(e->name, name)) {
            *out_addr = e->addr;
            cache->hits++;
            return 1;
        }
    }

    cache->misses++;
    return 0;
}

int netd_dns_cache_insert(netd_dns_cache_t* cache, const char* name, uint32_t addr, uint32_t ttl_ms) {
    if (!cache || !name || !*name) {
        return 0;
    }

    if (!cache->entries) {
        if (!netd_dns_cache_ensure_capacity(cache, NETD_DNS_CACHE_SIZE)) {
            return 0;
        }
    }

    if (ttl_ms == 0) {
        ttl_ms = NETD_DNS_CACHE_TTL_MS;
    }

    for (uint32_t i = 0; i < cache->count; i++) {
        netd_dns_cache_entry_t* e = &cache->entries[i];

        if (netd_dns_name_equal(e->name, name)) {
            e->addr = addr;
            e->timestamp_ms = uptime_ms();
            e->ttl_ms = ttl_ms;
            return 1;
        }
    }

    netd_dns_cache_expire_old(cache);

    if (cache->count >= cache->capacity) {
        if (cache->capacity >= NETD_DNS_CACHE_SIZE * 4) {
            if (cache->count > 0) {
                cache->entries[0] = cache->entries[cache->count - 1];
                cache->count--;
            }
        } else {
            if (!netd_dns_cache_ensure_capacity(cache, cache->capacity + 1)) {
                return 0;
            }
        }
    }

    if (cache->count >= cache->capacity) {
        return 0;
    }

    netd_dns_cache_entry_t* e = &cache->entries[cache->count];
    memset(e, 0, sizeof(*e));

    size_t name_len = strlen(name);
    if (name_len > sizeof(e->name) - 1) {
        name_len = sizeof(e->name) - 1;
    }

    memcpy(e->name, name, name_len);
    e->name[name_len] = '\0';
    e->addr = addr;
    e->timestamp_ms = uptime_ms();
    e->ttl_ms = ttl_ms;

    cache->count++;
    return 1;
}

void netd_dns_cache_clear(netd_dns_cache_t* cache) {
    if (!cache) {
        return;
    }

    cache->count = 0;
}

uint32_t netd_dns_cache_size(const netd_dns_cache_t* cache) {
    if (!cache) {
        return 0;
    }
    return cache->count;
}

void netd_dns_cache_print(const netd_dns_cache_t* cache) {
    if (!cache) {
        return;
    }

    printf("DNS Cache (%u entries, capacity %u):\n", cache->count, cache->capacity);
    printf("Statistics: %u hits, %u misses\n", cache->hits, cache->misses);

    if (cache->count == 0) {
        printf("  (empty)\n");
        return;
    }

    uint32_t now = uptime_ms();

    for (uint32_t i = 0; i < cache->count; i++) {
        const netd_dns_cache_entry_t* e = &cache->entries[i];

        uint32_t age_sec = (now - e->timestamp_ms) / 1000;
        uint32_t ttl_sec = e->ttl_ms / 1000;

        printf("  %s -> %u.%u.%u.%u (age: %us, ttl: %us)\n",
               e->name,
               (e->addr >> 24) & 0xFF,
               (e->addr >> 16) & 0xFF,
               (e->addr >> 8) & 0xFF,
               e->addr & 0xFF,
               age_sec, ttl_sec);
    }
}