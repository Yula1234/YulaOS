#include "dns_client.h"

#include "arena.h"
#include "netdev.h"
#include "arp.h"
#include "dns_transport.h"
#include "dns_wire.h"

#include <yula.h>

namespace netd {

namespace {

static constexpr uint16_t kDnsPort = 53u;
static constexpr uint32_t kMaxOps = 64u;

}

DnsClient::DnsClient(Arena& arena, NetDev& dev, Arp& arp)
    : m_dev(dev),
      m_arp(arp),
      m_cfg{},
      m_ops(arena),
      m_key_to_index(arena),
      m_results(arena),
      m_next_txid(1u) {
    (void)m_ops.reserve(16u);
    (void)m_results.reserve(16u);
}

void DnsClient::set_config(const DnsConfig& cfg) {
    m_cfg = cfg;
}

uint64_t DnsClient::make_key(uint32_t client_token, uint32_t tag) {
    const uint64_t k = ((uint64_t)client_token << 32) | (uint64_t)tag;
    if (k == 0ull) {
        return 1ull;
    }

    return k;
}

bool DnsClient::submit_resolve(const ResolveRequest& req, uint32_t now_ms) {
    if (req.name_len == 0u || req.name_len > 127u) {
        return false;
    }

    if (m_ops.size() >= kMaxOps) {
        return false;
    }

    Op op{};
    op.key = make_key(req.client_token, req.tag);
    op.tag = req.tag;
    op.client_token = req.client_token;

    {
        uint32_t existing = 0;
        if (m_key_to_index.get(op.key, existing)) {
            return false;
        }
    }

    op.dst_ip_be = m_cfg.dns_ip_be;
    op.next_hop_ip_be = m_cfg.gw_be;

    const uint32_t timeout = req.timeout_ms ? req.timeout_ms : 2000u;

    op.deadline_ms = now_ms + timeout;
    op.next_arp_tx_ms = now_ms;

    op.next_tx_ms = now_ms;
    op.tries = 0u;

    op.txid = m_next_txid;
    m_next_txid++;
    if (m_next_txid == 0u) {
        m_next_txid = 1u;
    }

    op.src_port = dns_transport::alloc_src_port(now_ms);

    op.dst_mac = Mac{};

    op.name_len = req.name_len;
    memcpy(op.name, req.name, req.name_len);

    op.state = 0u;

    if (!m_ops.push_back(op)) {
        return false;
    }

    (void)m_key_to_index.put(op.key, m_ops.size() - 1u);

    return true;
}

void DnsClient::complete_op(uint32_t op_index, uint32_t ip_be, uint8_t ok) {
    if (op_index >= m_ops.size()) {
        return;
    }

    const uint32_t last = m_ops.size() - 1u;
    const uint64_t removed_key = m_ops[op_index].key;
    const uint64_t moved_key = (op_index != last) ? m_ops[last].key : 0ull;

    const Op op = m_ops[op_index];

    ResolveResult r{};
    r.ip_be = ip_be;
    r.ok = ok;
    r.tag = op.tag;
    r.client_token = op.client_token;

    (void)m_results.push_back(r);

    (void)m_key_to_index.erase(removed_key);
    m_ops.erase_unordered(op_index);

    if (moved_key != 0ull) {
        (void)m_key_to_index.put(moved_key, op_index);
    }
}

bool DnsClient::try_send_query(Op& op, uint32_t now_ms) {
    return dns_transport::send_a_query(
        m_dev,
        m_cfg,
        op.dst_mac,
        op.dst_ip_be,
        op.src_port,
        op.txid,
        op.name,
        op.name_len,
        now_ms
    );
}

void DnsClient::step(uint32_t now_ms) {
    for (uint32_t i = 0; i < m_ops.size();) {
        Op& op = m_ops[i];

        if (now_ms >= op.deadline_ms) {
            complete_op(i, 0u, 0u);
            continue;
        }

        if (op.state == 0u) {
            Mac mac{};
            if (m_arp.cache().lookup(op.next_hop_ip_be, mac, now_ms) && !mac_is_zero(mac)) {
                op.dst_mac = mac;
                op.state = 1u;
                op.next_tx_ms = now_ms;
                i++;
                continue;
            }

            if (now_ms >= op.next_arp_tx_ms) {
                (void)m_arp.request(op.next_hop_ip_be);
                op.next_arp_tx_ms = now_ms + 200u;
            }

            i++;
            continue;
        }

        if (op.state == 1u) {
            if (op.tries >= 3u) {
                i++;
                continue;
            }

            if (now_ms < op.next_tx_ms) {
                i++;
                continue;
            }

            if (!try_send_query(op, now_ms)) {
                op.tries = 3u;
                i++;
                continue;
            }

            op.tries++;
            op.next_tx_ms = now_ms + 800u;

            i++;
            continue;
        }

        i++;
    }
}

bool DnsClient::handle_udp_frame(
    const EthHdr* eth,
    const Ipv4Hdr* ip,
    const UdpHdr* udp,
    const uint8_t* payload,
    uint32_t payload_len,
    uint32_t now_ms
) {
    (void)eth;

    if (!ip || !udp || !payload) {
        return false;
    }

    if (ntohs(udp->src_port) != kDnsPort) {
        return false;
    }

    if (payload_len < 12u) {
        return false;
    }

    const uint16_t txid = (uint16_t)payload[0] << 8 | (uint16_t)payload[1];

    for (uint32_t i = 0; i < m_ops.size(); i++) {
        Op& op = m_ops[i];

        if (op.dst_ip_be != ip->src) {
            continue;
        }

        if (op.txid != txid) {
            continue;
        }

        if (htons(op.src_port) != udp->dst_port) {
            continue;
        }

        uint32_t ip_be = 0;
        if (dns_wire::parse_dns_a_response(txid, payload, payload_len, ip_be)) {
            complete_op(i, ip_be, 1u);
            return true;
        }

        complete_op(i, 0u, 0u);
        return true;
    }

    return false;
}

bool DnsClient::udp_proto_handler(
    void* ctx,
    const EthHdr* eth,
    const Ipv4Hdr* ip,
    const uint8_t* payload,
    uint32_t payload_len,
    uint32_t now_ms
) {
    DnsClient* self = (DnsClient*)ctx;
    if (!self || !eth || !ip || !payload) {
        return false;
    }

    if (payload_len < (uint32_t)sizeof(UdpHdr)) {
        return false;
    }

    const UdpHdr* udp = (const UdpHdr*)payload;

    const uint32_t udp_len = (uint32_t)ntohs(udp->len);
    if (udp_len < sizeof(UdpHdr)) {
        return false;
    }

    if (udp_len > payload_len) {
        return false;
    }

    const uint8_t* udp_payload = payload + sizeof(UdpHdr);
    const uint32_t udp_payload_len = udp_len - (uint32_t)sizeof(UdpHdr);

    return self->handle_udp_frame(eth, ip, udp, udp_payload, udp_payload_len, now_ms);
}

bool DnsClient::poll_result(ResolveResult& out) {
    if (m_results.size() == 0) {
        return false;
    }

    out = m_results[0];
    m_results.erase_unordered(0);
    return true;
}

}
