#ifndef YOS_NETD_ARP_H
#define YOS_NETD_ARP_H

#include "net_core.h"
#include "net_proto.h"
#include "netdev.h"
#include "net_vec.h"

#include <stdint.h>

namespace netd {

struct ArpEntry {
    uint32_t ip_be;
    Mac mac;
    uint32_t last_seen_ms;
};

class ArpCache {
public:
    static constexpr uint32_t kInitialReserve = 64;
    static constexpr uint32_t kSoftMaxEntries = 1024;
    static constexpr uint32_t kTtlMs = 5u * 60u * 1000u;

    explicit ArpCache(Arena& arena);

    bool lookup(uint32_t ip_be, Mac& out_mac, uint32_t now_ms) const;
    void upsert(uint32_t ip_be, const Mac& mac, uint32_t now_ms);
    void prune(uint32_t now_ms);

private:
    Vector<ArpEntry> m_entries;
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
    bool resolve(uint32_t ip_be, Mac& out_mac, uint32_t timeout_ms);

    bool request(uint32_t target_ip_be);

    const ArpCache& cache() const { return m_cache; }

private:
    bool send_request(uint32_t target_ip_be);
    bool send_reply(const Mac& dst_mac, uint32_t dst_ip_be);

    NetDev& m_dev;
    ArpConfig m_cfg;
    ArpCache m_cache;
};

}

#endif
