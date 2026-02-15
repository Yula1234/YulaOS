#include "dns_client.h"

#include "arena.h"
#include "ipv4.h"
#include "udp.h"
#include "arp.h"
#include "dns_wire.h"

#include <yula.h>

namespace netd {

namespace {

static constexpr uint16_t kDnsPort = 53u;
static constexpr uint32_t kMaxOps = 64u;

static uint16_t rand16(uint32_t now_ms) {
    const uint32_t t = now_ms;
    return (uint16_t)((t & 0xFFFFu) ^ (t >> 16));
}

}

DnsClient::DnsClient(Arena& arena, Ipv4& ipv4, Udp& udp, Arp& arp)
    : m_ipv4(ipv4),
      m_udp(udp),
      m_arp(arp),
      m_cfg{},
      m_ops(arena),
      m_key_to_index(arena),
      m_resp_key_to_index(arena),
      m_results(arena),
      m_next_txid(1u),
      m_wakeup() {
    (void)m_ops.reserve(16u);
    (void)m_results.reserve(16u);
}

uint32_t DnsClient::op_next_wakeup_ms(const Op& op) {
    uint32_t t = op.deadline_ms;

    if (op.state == OpState::ArpWait) {
        if (op.arp_wait.next_tx_ms() < t) {
            t = op.arp_wait.next_tx_ms();
        }
    } else if (op.state == OpState::Tx) {
        if (op.next_tx_ms < t) {
            t = op.next_tx_ms;
        }
    }

    return t;
}

bool DnsClient::try_get_next_wakeup_ms(uint32_t now_ms, uint32_t& out_ms) const {
    const Span<const Op> ops(m_ops.data(), m_ops.size());
    return m_wakeup.try_get_next(ops, now_ms, out_ms, &DnsClient::op_next_wakeup_ms);
}

void DnsClient::set_config(const DnsConfig& cfg) {
    m_cfg = cfg;
}

uint16_t DnsClient::alloc_src_port(uint32_t now_ms) {
    return (uint16_t)(40000u + (rand16(now_ms) % 20000u));
}

uint64_t DnsClient::make_key(uint32_t client_token, uint32_t tag) {
    const uint64_t k = ((uint64_t)client_token << 32) | (uint64_t)tag;
    if (k == 0ull) {
        return 1ull;
    }

    return k;
}

uint64_t DnsClient::make_resp_key(uint32_t src_ip_be, uint16_t src_port, uint16_t txid) {
    const uint64_t ip = (uint64_t)src_ip_be;
    const uint64_t p = (uint64_t)src_port;
    const uint64_t t = (uint64_t)txid;
    return (ip << 32) | (p << 16) | t;
}

uint64_t DnsClient::make_resp_key(const Op& op) {
    return make_resp_key(op.dst_ip_be, op.src_port, op.txid);
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

    op.dst_ip_be = m_cfg.dns_ip_be;
    op.next_hop_ip_be = m_ipv4.next_hop_ip(op.dst_ip_be);

    const uint32_t timeout = req.timeout_ms ? req.timeout_ms : 2000u;

    op.deadline_ms = now_ms + timeout;

    op.next_tx_ms = now_ms;
    op.tries = 0u;

    op.txid = m_next_txid;
    m_next_txid++;
    if (m_next_txid == 0u) {
        m_next_txid = 1u;
    }

    op.src_port = alloc_src_port(now_ms);

    op.arp_wait.reset(now_ms);

    op.name_len = req.name_len;
    memcpy(op.name, req.name, req.name_len);

    op.state = OpState::ArpWait;

    const uint64_t resp_key = make_resp_key(op);
    {
        uint32_t existing = 0;
        if (m_key_to_index.get(op.key, existing)) {
            return false;
        }
        if (m_resp_key_to_index.get(resp_key, existing)) {
            return false;
        }
    }

    if (!m_ops.push_back(op)) {
        return false;
    }

    const uint32_t op_index = m_ops.size() - 1u;

    if (!m_key_to_index.put(op.key, op_index)) {
        m_ops.erase_unordered(op_index);
        return false;
    }

    if (!m_resp_key_to_index.put(resp_key, op_index)) {
        (void)m_key_to_index.erase(op.key);
        m_ops.erase_unordered(op_index);
        return false;
    }

    const uint32_t wake = op_next_wakeup_ms(op);
    m_wakeup.update_candidate(wake);

    return true;
}

void DnsClient::complete_op(uint32_t op_index, uint32_t ip_be, uint8_t ok, uint32_t now_ms) {
    if (op_index >= m_ops.size()) {
        return;
    }

    const uint32_t last = m_ops.size() - 1u;

    const Op op = m_ops[op_index];
    const uint64_t removed_key = op.key;
    const uint64_t removed_resp_key = make_resp_key(op);

    ResolveResult r{};
    r.ip_be = ip_be;
    r.ok = ok;
    r.tag = op.tag;
    r.client_token = op.client_token;

    (void)m_results.push_back(r);

    (void)m_key_to_index.erase(removed_key);
    (void)m_resp_key_to_index.erase(removed_resp_key);
    m_ops.erase_unordered(op_index);

    if (op_index != last) {
        const Op& moved = m_ops[op_index];
        (void)m_key_to_index.put(moved.key, op_index);
        (void)m_resp_key_to_index.put(make_resp_key(moved), op_index);
    }

    const Span<const Op> ops(m_ops.data(), m_ops.size());
    m_wakeup.recompute_if_due(ops, now_ms, &DnsClient::op_next_wakeup_ms);
}

bool DnsClient::try_send_query(Op& op, uint32_t now_ms) {
    uint8_t dns[256];
    uint32_t dns_len = 0;

    if (!dns_wire::build_dns_a_query(op.txid, op.name, op.name_len, dns, (uint32_t)sizeof(dns), dns_len)) {
        return false;
    }

    (void)now_ms;
    return m_udp.send_to(op.arp_wait.mac(), op.dst_ip_be, op.src_port, kDnsPort, dns, dns_len, now_ms);
}

void DnsClient::step(uint32_t now_ms) {
    for (uint32_t i = 0; i < m_ops.size();) {
        Op& op = m_ops[i];

        if (now_ms >= op.deadline_ms) {
            complete_op(i, 0u, 0u, now_ms);
            continue;
        }

        if (op.state == OpState::ArpWait) {
            if (op.arp_wait.step(m_arp, op.next_hop_ip_be, now_ms, 200u)) {
                op.state = OpState::Tx;
                op.next_tx_ms = now_ms;

                const uint32_t wake = op_next_wakeup_ms(op);
                m_wakeup.update_candidate(wake);

                i++;
                continue;
            }

            const uint32_t wake = op_next_wakeup_ms(op);
            m_wakeup.update_candidate(wake);

            i++;
            continue;
        }

        if (op.state == OpState::Tx) {
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

            {
                const uint32_t wake = op_next_wakeup_ms(op);
                m_wakeup.update_candidate(wake);
            }

            i++;
            continue;
        }

        i++;
    }

    {
        const Span<const Op> ops(m_ops.data(), m_ops.size());
        m_wakeup.recompute_if_due(ops, now_ms, &DnsClient::op_next_wakeup_ms);
    }
}

bool DnsClient::udp_port_handler(
    void* ctx,
    const Ipv4Hdr* ip,
    uint16_t src_port,
    uint16_t dst_port,
    const uint8_t* payload,
    uint32_t payload_len,
    uint32_t now_ms
) {
    DnsClient* self = (DnsClient*)ctx;
    if (!self || !ip || !payload) {
        return false;
    }

    if (src_port != kDnsPort) {
        return false;
    }

    if (dst_port == 0u) {
        return false;
    }

    if (payload_len < 12u) {
        return false;
    }

    const uint16_t txid = (uint16_t)payload[0] << 8 | (uint16_t)payload[1];
    const uint64_t key = make_resp_key(ip->src, dst_port, txid);

    uint32_t op_index = 0;
    if (!self->m_resp_key_to_index.get(key, op_index)) {
        return false;
    }

    if (op_index >= self->m_ops.size()) {
        return false;
    }

    const Op& op = self->m_ops[op_index];
    if (op.dst_ip_be != ip->src || op.txid != txid || op.src_port != dst_port) {
        return false;
    }

    uint32_t ip_be = 0;
    if (dns_wire::parse_dns_a_response(txid, payload, payload_len, ip_be)) {
        self->complete_op(op_index, ip_be, 1u, now_ms);
        return true;
    }

    self->complete_op(op_index, 0u, 0u, now_ms);
    return true;
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
