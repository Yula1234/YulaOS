#include "arp.h"

#include <yula.h>

namespace netd {

namespace {

static uint32_t find_oldest_index(const Vector<ArpEntry>& entries) {
    uint32_t oldest_i = 0;
    uint32_t oldest_t = entries.size() ? entries[0].last_seen_ms : 0u;

    for (uint32_t i = 1; i < entries.size(); i++) {
        const uint32_t t = entries[i].last_seen_ms;
        if (t < oldest_t) {
            oldest_t = t;
            oldest_i = i;
        }
    }

    return oldest_i;
}

}

ArpCache::ArpCache(Arena& arena) : m_entries(arena) {
    (void)m_entries.reserve(kInitialReserve);
}

bool ArpCache::lookup(uint32_t ip_be, Mac& out_mac, uint32_t now_ms) const {
    for (uint32_t i = 0; i < m_entries.size(); i++) {
        const ArpEntry& e = m_entries[i];

        if (e.ip_be != ip_be) {
            continue;
        }

        if ((now_ms - e.last_seen_ms) > kTtlMs) {
            return false;
        }

        out_mac = e.mac;
        return true;
    }

    return false;
}

void ArpCache::upsert(uint32_t ip_be, const Mac& mac, uint32_t now_ms) {
    for (uint32_t i = 0; i < m_entries.size(); i++) {
        ArpEntry& e = m_entries[i];

        if (e.ip_be == ip_be) {
            e.mac = mac;
            e.last_seen_ms = now_ms;
            return;
        }
    }

    if (m_entries.size() >= kSoftMaxEntries) {
        const uint32_t oldest_i = find_oldest_index(m_entries);
        m_entries[oldest_i] = ArpEntry{ ip_be, mac, now_ms };
        return;
    }

    if (!m_entries.push_back(ArpEntry{ ip_be, mac, now_ms })) {
        if (m_entries.size() == 0) {
            return;
        }

        const uint32_t oldest_i = find_oldest_index(m_entries);
        m_entries[oldest_i] = ArpEntry{ ip_be, mac, now_ms };
    }
}

void ArpCache::prune(uint32_t now_ms) {
    for (uint32_t i = 0; i < m_entries.size();) {
        const ArpEntry& e = m_entries[i];

        if ((now_ms - e.last_seen_ms) > kTtlMs) {
            m_entries.erase_unordered(i);
            continue;
        }

        i++;
    }
}

Arp::Arp(Arena& arena, NetDev& dev) : m_dev(dev), m_cfg{}, m_cache(arena) {
}

void Arp::set_config(const ArpConfig& cfg) {
    m_cfg = cfg;
}

static Mac mac_from_bytes(const uint8_t b[6]) {
    Mac m{};

    for (int i = 0; i < 6; i++) {
        m.b[i] = b[i];
    }

    return m;
}

static void mac_to_bytes(const Mac& m, uint8_t out[6]) {
    for (int i = 0; i < 6; i++) {
        out[i] = m.b[i];
    }
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
    uint8_t buf[64];
    const uint32_t len = (uint32_t)(sizeof(EthHdr) + sizeof(ArpHdr));

    EthHdr* eth = (EthHdr*)buf;
    ArpHdr* arp = (ArpHdr*)(buf + sizeof(EthHdr));

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

    return m_dev.write_frame(buf, len) > 0;
}

bool Arp::request(uint32_t target_ip_be) {
    return send_request(target_ip_be);
}

bool Arp::send_reply(const Mac& dst_mac, uint32_t dst_ip_be) {
    uint8_t buf[64];
    const uint32_t len = (uint32_t)(sizeof(EthHdr) + sizeof(ArpHdr));

    EthHdr* eth = (EthHdr*)buf;
    ArpHdr* arp = (ArpHdr*)(buf + sizeof(EthHdr));

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

    return m_dev.write_frame(buf, len) > 0;
}

bool Arp::resolve(uint32_t ip_be, Mac& out_mac, uint32_t timeout_ms) {
    const uint32_t start = uptime_ms();
    uint32_t next_tx_ms = start;
    uint8_t frame[1600];

    for (;;) {
        const uint32_t now = uptime_ms();
        m_cache.prune(now);

        if (m_cache.lookup(ip_be, out_mac, now)) {
            return true;
        }

        if ((now - start) >= timeout_ms) {
            return false;
        }

        if (now >= next_tx_ms) {
            if (!send_request(ip_be)) {
                return false;
            }

            next_tx_ms = now + 200u;
        }

        pollfd_t fds[1];
        fds[0].fd = m_dev.fd();
        fds[0].events = POLLIN;
        fds[0].revents = 0;

        uint32_t remain = timeout_ms - (now - start);
        int wait_ms = 50;
        if (remain < (uint32_t)wait_ms) {
            wait_ms = (int)remain;
        }

        const int pr = poll(fds, 1, wait_ms);
        if (pr <= 0) {
            continue;
        }

        for (;;) {
            const int r = m_dev.read_frame(frame, (uint32_t)sizeof(frame));
            if (r <= 0) {
                break;
            }

            (void)handle_frame(frame, (uint32_t)r, uptime_ms());
        }
    }
}

}
