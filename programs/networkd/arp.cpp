#include "arp.h"

#include "arena.h"
#include "net_packet_builder.h"
#include "netdev.h"
#include "net_mac.h"

#include <yula.h>

namespace netd {

ArpCache::ArpCache(Arena& arena)
    : m_arena(arena),
      m_table(arena),
      m_lru_head(nullptr),
      m_lru_tail(nullptr),
      m_count(0) {
    (void)m_table.reserve(kInitialReserve);
}

ArpEntry* ArpCache::alloc_entry(uint32_t ip_be, const Mac& mac, uint32_t now_ms) {
    void* mem = m_arena.alloc((uint32_t)sizeof(ArpEntry), (uint32_t)alignof(ArpEntry));
    if (!mem) {
        return nullptr;
    }

    ArpEntry* entry = (ArpEntry*)mem;
    entry->ip_be = ip_be;
    entry->mac = mac;
    entry->last_seen_ms = now_ms;
    entry->lru_prev = nullptr;
    entry->lru_next = nullptr;

    return entry;
}

void ArpCache::unlink_from_lru(ArpEntry* entry) {
    if (!entry) {
        return;
    }

    if (entry->lru_prev) {
        entry->lru_prev->lru_next = entry->lru_next;
    } else {
        m_lru_head = entry->lru_next;
    }

    if (entry->lru_next) {
        entry->lru_next->lru_prev = entry->lru_prev;
    } else {
        m_lru_tail = entry->lru_prev;
    }

    entry->lru_prev = nullptr;
    entry->lru_next = nullptr;
}

void ArpCache::push_to_lru_head(ArpEntry* entry) {
    if (!entry) {
        return;
    }

    entry->lru_prev = nullptr;
    entry->lru_next = m_lru_head;

    if (m_lru_head) {
        m_lru_head->lru_prev = entry;
    } else {
        m_lru_tail = entry;
    }

    m_lru_head = entry;
}

void ArpCache::touch_entry(ArpEntry* entry, uint32_t now_ms) {
    if (!entry) {
        return;
    }

    entry->last_seen_ms = now_ms;
    
    unlink_from_lru(entry);
    push_to_lru_head(entry);
}

ArpEntry* ArpCache::evict_lru() {
    if (!m_lru_tail) {
        return nullptr;
    }

    ArpEntry* victim = m_lru_tail;
    const uint32_t victim_ip = victim->ip_be;

    unlink_from_lru(victim);
    (void)m_table.erase(victim_ip);

    if (m_count > 0) {
        m_count--;
    }

    return victim;
}

bool ArpCache::is_expired(const ArpEntry* entry, uint32_t now_ms) const {
    if (!entry) {
        return true;
    }

    return (now_ms - entry->last_seen_ms) > kTtlMs;
}

bool ArpCache::lookup(uint32_t ip_be, Mac& out_mac, uint32_t now_ms) {
    ArpEntry* entry = nullptr;
    if (!m_table.get(ip_be, entry) || !entry) {
        return false;
    }

    if (is_expired(entry, now_ms)) {
        unlink_from_lru(entry);
        (void)m_table.erase(ip_be);
        
        if (m_count > 0) {
            m_count--;
        }
        
        return false;
    }

    touch_entry(entry, now_ms);
    out_mac = entry->mac;
    return true;
}

void ArpCache::upsert(uint32_t ip_be, const Mac& mac, uint32_t now_ms) {
    ArpEntry* existing = nullptr;
    
    if (m_table.get(ip_be, existing) && existing) {
        existing->mac = mac;
        touch_entry(existing, now_ms);
        return;
    }

    if (m_count >= kSoftMaxEntries) {
        ArpEntry* victim = evict_lru();
        if (!victim) {
            return;
        }

        victim->ip_be = ip_be;
        victim->mac = mac;
        victim->last_seen_ms = now_ms;
        victim->lru_prev = nullptr;
        victim->lru_next = nullptr;

        if (!m_table.put(ip_be, victim)) {
            return;
        }

        push_to_lru_head(victim);
        m_count++;
        return;
    }

    ArpEntry* new_entry = alloc_entry(ip_be, mac, now_ms);
    if (!new_entry) {
        return;
    }

    if (!m_table.put(ip_be, new_entry)) {
        return;
    }

    push_to_lru_head(new_entry);
    m_count++;
}

void ArpCache::prune(uint32_t now_ms) {
    ArpEntry* current = m_lru_tail;
    
    while (current) {
        ArpEntry* prev = current->lru_prev;
        
        if (is_expired(current, now_ms)) {
            const uint32_t ip = current->ip_be;
            
            unlink_from_lru(current);
            (void)m_table.erase(ip);
            
            if (m_count > 0) {
                m_count--;
            }
        } else {
            break;
        }
        
        current = prev;
    }
}

Arp::Arp(Arena& arena, NetDev& dev) : m_dev(dev), m_cfg{}, m_cache(arena) {
}

void Arp::set_config(const ArpConfig& cfg) {
    m_cfg = cfg;
}

bool Arp::handle_frame(const uint8_t* frame, uint32_t len, uint32_t now_ms) {
    if (!frame || len < (uint32_t)(sizeof(EthHdr) + sizeof(ArpHdr))) {
        return false;
    }

    const EthHdr* eth = (const EthHdr*)frame;
    if (ntohs(eth->ethertype) != ETHERTYPE_ARP) {
        return false;
    }

    const ArpHdr* arp = (const ArpHdr*)(frame + sizeof(EthHdr));
    if (ntohs(arp->htype) != ARP_HTYPE_ETH) {
        return true;
    }

    if (ntohs(arp->ptype) != ETHERTYPE_IPV4) {
        return true;
    }

    if (arp->hlen != 6u || arp->plen != 4u) {
        return true;
    }

    const Mac sha = mac_from_bytes(arp->sha);
    m_cache.upsert(arp->spa, sha, now_ms);

    const uint16_t oper = ntohs(arp->oper);
    if (oper == ARP_OPER_REQUEST) {
        if (arp->tpa == m_cfg.ip_be) {
            (void)send_reply(sha, arp->spa);
        }

        return true;
    }

    return true;
}

bool Arp::send_request(uint32_t target_ip_be) {
    PacketBuilder pb;
    ArpHdr* arp = (ArpHdr*)pb.append((uint32_t)sizeof(ArpHdr));
    if (!arp) {
        return false;
    }

    EthHdr* eth = (EthHdr*)pb.prepend((uint32_t)sizeof(EthHdr));
    if (!eth) {
        return false;
    }

    const Mac bcast = mac_broadcast();
    mac_to_bytes(bcast, eth->dst);
    mac_to_bytes(m_cfg.mac, eth->src);
    eth->ethertype = htons(ETHERTYPE_ARP);

    arp->htype = htons(ARP_HTYPE_ETH);
    arp->ptype = htons(ETHERTYPE_IPV4);
    arp->hlen = 6;
    arp->plen = 4;
    arp->oper = htons(ARP_OPER_REQUEST);
    mac_to_bytes(m_cfg.mac, arp->sha);
    arp->spa = m_cfg.ip_be;

    for (int i = 0; i < 6; i++) {
        arp->tha[i] = 0;
    }

    arp->tpa = target_ip_be;

    return m_dev.write_frame(pb.data(), pb.size()) > 0;
}

bool Arp::request(uint32_t target_ip_be) {
    return send_request(target_ip_be);
}

bool Arp::send_reply(const Mac& dst_mac, uint32_t dst_ip_be) {
    PacketBuilder pb;
    ArpHdr* arp = (ArpHdr*)pb.append((uint32_t)sizeof(ArpHdr));
    if (!arp) {
        return false;
    }

    EthHdr* eth = (EthHdr*)pb.prepend((uint32_t)sizeof(EthHdr));
    if (!eth) {
        return false;
    }

    mac_to_bytes(dst_mac, eth->dst);
    mac_to_bytes(m_cfg.mac, eth->src);
    eth->ethertype = htons(ETHERTYPE_ARP);

    arp->htype = htons(ARP_HTYPE_ETH);
    arp->ptype = htons(ETHERTYPE_IPV4);
    arp->hlen = 6;
    arp->plen = 4;
    arp->oper = htons(ARP_OPER_REPLY);
    mac_to_bytes(m_cfg.mac, arp->sha);
    arp->spa = m_cfg.ip_be;
    mac_to_bytes(dst_mac, arp->tha);
    arp->tpa = dst_ip_be;

    return m_dev.write_frame(pb.data(), pb.size()) > 0;
}

ArpWaitState::ArpWaitState() : m_mac{}, m_next_tx_ms(0u) {
}

void ArpWaitState::reset(uint32_t now_ms) {
    m_mac = Mac{};
    m_next_tx_ms = now_ms;
}

bool ArpWaitState::step(Arp& arp, uint32_t ip_be, uint32_t now_ms, uint32_t retry_ms) {
    Mac mac{};
    if (arp.cache().lookup(ip_be, mac, now_ms) && !mac_is_zero(mac)) {
        m_mac = mac;
        return true;
    }

    if (now_ms >= m_next_tx_ms) {
        (void)arp.request(ip_be);
        m_next_tx_ms = now_ms + retry_ms;
    }

    return false;
}

const Mac& ArpWaitState::mac() const {
    return m_mac;
}

uint32_t ArpWaitState::next_tx_ms() const {
    return m_next_tx_ms;
}

}
