// SPDX-License-Identifier: GPL-2.0

#include "netd_arp.h"

#include <yula.h>

#include "netd_config.h"
#include "netd_device.h"
#include "netd_iface.h"
#include "netd_proto.h"
#include "netd_stats.h"
#include "netd_util.h"

static int netd_arp_cache_ensure_capacity(netd_arp_cache_t* cache, uint32_t min_cap) {
    if (!cache) {
        return 0;
    }

    if (cache->capacity >= min_cap) {
        return 1;
    }

    uint32_t new_cap = cache->capacity == 0 ? NETD_ARP_CACHE_INITIAL : cache->capacity;
    while (new_cap < min_cap && new_cap < NETD_ARP_CACHE_MAX) {
        new_cap *= 2;
    }

    if (new_cap > NETD_ARP_CACHE_MAX) {
        new_cap = NETD_ARP_CACHE_MAX;
    }

    if (new_cap < min_cap) {
        return 0;
    }

    netd_arp_entry_t* new_entries = (netd_arp_entry_t*)realloc(
        cache->entries, 
        new_cap * sizeof(netd_arp_entry_t)
    );
    
    if (!new_entries) {
        return 0;
    }

    if (new_cap > cache->capacity) {
        memset(
            new_entries + cache->capacity, 
            0, 
            (new_cap - cache->capacity) * sizeof(netd_arp_entry_t)
        );
    }

    cache->entries = new_entries;
    cache->capacity = new_cap;
    return 1;
}

static void netd_arp_cache_expire_old(netd_arp_cache_t* cache) {
    if (!cache || !cache->entries) {
        return;
    }

    uint32_t now = uptime_ms();
    uint32_t i = 0;
    
    while (i < cache->count) {
        netd_arp_entry_t* e = &cache->entries[i];
        
        if (e->used && e->ttl_ms > 0) {
            uint32_t age = now - e->timestamp_ms;
            if (age >= e->ttl_ms) {
                e->used = 0;
                
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

static int netd_arp_cache_lookup(netd_arp_cache_t* cache, uint32_t ip, uint8_t out_mac[6]) {
    if (!cache || !cache->entries || !out_mac) {
        return 0;
    }

    netd_arp_cache_expire_old(cache);

    for (uint32_t i = 0; i < cache->count; i++) {
        netd_arp_entry_t* e = &cache->entries[i];
        if (!e->used) {
            continue;
        }
        if (e->ip != ip) {
            continue;
        }
        
        memcpy(out_mac, e->mac, 6);
        cache->hits++;
        return 1;
    }

    cache->misses++;
    return 0;
}

static void netd_arp_cache_update(netd_arp_cache_t* cache, uint32_t ip, const uint8_t mac[6]) {
    if (!cache || !mac) {
        return;
    }

    if (!cache->entries) {
        if (!netd_arp_cache_ensure_capacity(cache, NETD_ARP_CACHE_INITIAL)) {
            return;
        }
    }

    for (uint32_t i = 0; i < cache->count; i++) {
        netd_arp_entry_t* e = &cache->entries[i];
        if (!e->used) {
            continue;
        }
        if (e->ip != ip) {
            continue;
        }
        
        memcpy(e->mac, mac, 6);
        e->timestamp_ms = uptime_ms();
        e->ttl_ms = NETD_ARP_ENTRY_TTL_MS;
        return;
    }

    netd_arp_cache_expire_old(cache);

    if (cache->count >= cache->capacity) {
        if (cache->capacity >= NETD_ARP_CACHE_MAX) {
            uint32_t victim = cache->next_slot % cache->count;
            cache->next_slot++;
            
            netd_arp_entry_t* e = &cache->entries[victim];
            e->used = 1;
            e->ip = ip;
            memcpy(e->mac, mac, 6);
            e->timestamp_ms = uptime_ms();
            e->ttl_ms = NETD_ARP_ENTRY_TTL_MS;
            return;
        }

        if (!netd_arp_cache_ensure_capacity(cache, cache->capacity + 1)) {
            return;
        }
    }

    netd_arp_entry_t* e = &cache->entries[cache->count];
    e->used = 1;
    e->ip = ip;
    memcpy(e->mac, mac, 6);
    e->timestamp_ms = uptime_ms();
    e->ttl_ms = NETD_ARP_ENTRY_TTL_MS;
    cache->count++;
}

static int netd_send_arp_request(netd_ctx_t* ctx, uint32_t target_ip) {
    if (!ctx || !ctx->iface.up) {
        return -1;
    }

    net_eth_hdr_t* eth = (net_eth_hdr_t*)ctx->tx_buf;
    net_arp_t* arp = (net_arp_t*)(ctx->tx_buf + sizeof(net_eth_hdr_t));

    memset(eth->dst, 0xFF, 6);
    memcpy(eth->src, ctx->iface.mac, 6);
    eth->ethertype = netd_htons(0x0806u);

    arp->htype = netd_htons(1);
    arp->ptype = netd_htons(0x0800u);
    arp->hlen = 6;
    arp->plen = 4;
    arp->opcode = netd_htons(1);
    memcpy(arp->sha, ctx->iface.mac, 6);
    arp->spa = netd_htonl(ctx->iface.ip);
    memset(arp->tha, 0, 6);
    arp->tpa = netd_htonl(target_ip);

    uint32_t len = sizeof(net_eth_hdr_t) + sizeof(net_arp_t);
    
    if (ctx->enable_stats) {
        netd_stats_arp_request(&ctx->stats);
    }

    return netd_iface_send_frame(ctx, ctx->tx_buf, len);
}

static int netd_send_arp_reply(netd_ctx_t* ctx, uint32_t target_ip, const uint8_t target_mac[6]) {
    if (!ctx || !ctx->iface.up || !target_mac) {
        return -1;
    }

    net_eth_hdr_t* eth = (net_eth_hdr_t*)ctx->tx_buf;
    net_arp_t* arp = (net_arp_t*)(ctx->tx_buf + sizeof(net_eth_hdr_t));

    memcpy(eth->dst, target_mac, 6);
    memcpy(eth->src, ctx->iface.mac, 6);
    eth->ethertype = netd_htons(0x0806u);

    arp->htype = netd_htons(1);
    arp->ptype = netd_htons(0x0800u);
    arp->hlen = 6;
    arp->plen = 4;
    arp->opcode = netd_htons(2);
    memcpy(arp->sha, ctx->iface.mac, 6);
    arp->spa = netd_htonl(ctx->iface.ip);
    memcpy(arp->tha, target_mac, 6);
    arp->tpa = netd_htonl(target_ip);

    uint32_t len = sizeof(net_eth_hdr_t) + sizeof(net_arp_t);
    
    if (ctx->enable_stats) {
        netd_stats_arp_reply(&ctx->stats);
    }

    return netd_iface_send_frame(ctx, ctx->tx_buf, len);
}

void netd_arp_init(netd_ctx_t* ctx) {
    if (!ctx) {
        return;
    }

    memset(&ctx->arp_cache, 0, sizeof(ctx->arp_cache));
    
    if (!netd_arp_cache_ensure_capacity(&ctx->arp_cache, NETD_ARP_CACHE_INITIAL)) {
        netd_log_warn(ctx, "Failed to allocate initial ARP cache");
    }
}

void netd_arp_cleanup(netd_ctx_t* ctx) {
    if (!ctx) {
        return;
    }

    if (ctx->arp_cache.entries) {
        free(ctx->arp_cache.entries);
        ctx->arp_cache.entries = NULL;
    }

    ctx->arp_cache.count = 0;
    ctx->arp_cache.capacity = 0;
}

void netd_arp_process_frame(netd_ctx_t* ctx, const uint8_t* buf, uint32_t len) {
    if (!ctx || !buf) {
        return;
    }

    if (len < sizeof(net_eth_hdr_t) + sizeof(net_arp_t)) {
        return;
    }

    const net_arp_t* arp = (const net_arp_t*)(buf + sizeof(net_eth_hdr_t));

    if (netd_ntohs(arp->htype) != 1) {
        return;
    }
    if (netd_ntohs(arp->ptype) != 0x0800u) {
        return;
    }
    if (arp->hlen != 6 || arp->plen != 4) {
        return;
    }

    uint32_t spa = netd_ntohl(arp->spa);
    uint32_t tpa = netd_ntohl(arp->tpa);

    netd_arp_cache_update(&ctx->arp_cache, spa, arp->sha);

    uint16_t opcode = netd_ntohs(arp->opcode);
    
    if (opcode == 1 && tpa == ctx->iface.ip) {
        netd_send_arp_reply(ctx, spa, arp->sha);
        return;
    }
    
    if (opcode == 2 && tpa == ctx->iface.ip) {
        if (ctx->enable_stats) {
            netd_stats_arp_reply(&ctx->stats);
        }
    }
}

int netd_arp_resolve_mac(netd_ctx_t* ctx, uint32_t target_ip, uint8_t out_mac[6], uint32_t timeout_ms) {
    if (!ctx || !ctx->iface.up || !out_mac) {
        return 0;
    }

    if (netd_arp_cache_lookup(&ctx->arp_cache, target_ip, out_mac)) {
        if (ctx->enable_stats) {
            netd_stats_arp_cache_hit(&ctx->stats);
        }
        return 1;
    }

    if (ctx->enable_stats) {
        netd_stats_arp_cache_miss(&ctx->stats);
    }

    uint32_t elapsed = 0;
    uint32_t step_ms = NETD_POLL_TIMEOUT_MS;
    uint32_t next_send = 0;
    uint32_t retries = 0;

    while (elapsed < timeout_ms && retries < NETD_ARP_RETRY_COUNT) {
        if (elapsed >= next_send) {
            if (netd_send_arp_request(ctx, target_ip) < 0) {
                return 0;
            }
            next_send = elapsed + (timeout_ms / NETD_ARP_RETRY_COUNT);
            retries++;
        }

        netd_device_process(ctx);

        if (netd_arp_cache_lookup(&ctx->arp_cache, target_ip, out_mac)) {
            return 1;
        }

        sleep((int)step_ms);
        elapsed += step_ms;
    }

    if (ctx->enable_stats) {
        netd_stats_arp_timeout(&ctx->stats);
    }

    return 0;
}

void netd_arp_cache_clear(netd_ctx_t* ctx) {
    if (!ctx) {
        return;
    }

    netd_arp_cache_t* cache = &ctx->arp_cache;
    
    for (uint32_t i = 0; i < cache->count; i++) {
        cache->entries[i].used = 0;
    }
    
    cache->count = 0;
    cache->next_slot = 0;
}

uint32_t netd_arp_cache_size(const netd_ctx_t* ctx) {
    if (!ctx) {
        return 0;
    }
    return ctx->arp_cache.count;
}

void netd_arp_cache_print(const netd_ctx_t* ctx) {
    if (!ctx) {
        return;
    }

    const netd_arp_cache_t* cache = &ctx->arp_cache;
    
    printf("ARP Cache (%u entries, capacity %u):\n", cache->count, cache->capacity);
    printf("Statistics: %u hits, %u misses, %u timeouts\n", 
           cache->hits, cache->misses, cache->timeouts);
    
    if (cache->count == 0) {
        printf("  (empty)\n");
        return;
    }

    uint32_t now = uptime_ms();
    
    for (uint32_t i = 0; i < cache->count; i++) {
        const netd_arp_entry_t* e = &cache->entries[i];
        if (!e->used) {
            continue;
        }

        uint32_t age_sec = (now - e->timestamp_ms) / 1000;
        uint32_t ttl_sec = e->ttl_ms / 1000;
        
        printf("  %u.%u.%u.%u -> %02X:%02X:%02X:%02X:%02X:%02X (age: %us, ttl: %us)\n",
               (e->ip >> 24) & 0xFF,
               (e->ip >> 16) & 0xFF,
               (e->ip >> 8) & 0xFF,
               e->ip & 0xFF,
               e->mac[0], e->mac[1], e->mac[2],
               e->mac[3], e->mac[4], e->mac[5],
               age_sec, ttl_sec);
    }
}