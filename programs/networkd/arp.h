#ifndef YOS_NETD_ARP_H
#define YOS_NETD_ARP_H

#include "net_core.h"
#include "net_proto.h"
#include "net_hash_map.h"
#include "netdev.h"

#include <stdint.h>

namespace netd {

struct ArpEntry {
    uint32_t ip_be;
    Mac mac;
    uint32_t last_seen_ms;
    
    ArpEntry* lru_prev;
    ArpEntry* lru_next;
};

class ArpCache {
public:
    static constexpr uint32_t kInitialReserve = 64;
    static constexpr uint32_t kSoftMaxEntries = 1024;
    static constexpr uint32_t kTtlMs = 5u * 60u * 1000u;

    explicit ArpCache(Arena& arena);

    bool lookup(uint32_t ip_be, Mac& out_mac, uint32_t now_ms);
    void upsert(uint32_t ip_be, const Mac& mac, uint32_t now_ms);
    void prune(uint32_t now_ms);
    
    uint32_t size() const {
        return m_count;
    }

private:
    ArpEntry* alloc_entry(uint32_t ip_be, const Mac& mac, uint32_t now_ms);
    void touch_entry(ArpEntry* entry, uint32_t now_ms);
    void unlink_from_lru(ArpEntry* entry);
    void push_to_lru_head(ArpEntry* entry);
    ArpEntry* evict_lru();
    bool is_expired(const ArpEntry* entry, uint32_t now_ms) const;

    Arena& m_arena;
    HashMap<uint32_t, ArpEntry*> m_table;
    
    ArpEntry* m_lru_head;
    ArpEntry* m_lru_tail;
    
    uint32_t m_count;
};

struct ArpConfig {
    uint32_t ip_be;
    Mac mac;
};

class Arp {
public:
    Arp(Arena& arena, NetDev& dev);

    void set_config(const ArpConfig& cfg);

    bool handle_frame(const uint8_t* frame, uint32_t len, uint32_t now_ms);
    bool request(uint32_t target_ip_be);

    ArpCache& cache() {
        return m_cache;
    }
    
    const ArpCache& cache() const {
        return m_cache;
    }

private:
    bool send_request(uint32_t target_ip_be);
    bool send_reply(const Mac& dst_mac, uint32_t dst_ip_be);

    NetDev& m_dev;
    ArpConfig m_cfg;
    ArpCache m_cache;
};

struct ArpWaitState {
public:
    ArpWaitState();

    void reset(uint32_t now_ms);
    bool step(Arp& arp, uint32_t ip_be, uint32_t now_ms, uint32_t retry_ms);

    const Mac& mac() const;
    uint32_t next_tx_ms() const;

private:
    Mac m_mac;
    uint32_t m_next_tx_ms;
};

}

#endif
