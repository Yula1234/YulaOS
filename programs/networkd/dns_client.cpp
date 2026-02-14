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
      m_results(arena),
      m_next_txid(1u),
      m_next_wakeup_ms(0u) {
    (void)m_ops.reserve(16u);
    (void)m_results.reserve(16u);
}

uint32_t DnsClient::op_next_wakeup_ms(const Op& op) {
    uint32_t t = op.deadline_ms;

    if (op.state == 0u) {
        if (op.next_arp_tx_ms < t) {
            t = op.next_arp_tx_ms;
        }
    } else if (op.state == 1u) {
        if (op.next_tx_ms < t) {
            t = op.next_tx_ms;
        }
    }

    return t;
}

uint32_t DnsClient::recompute_next_wakeup_ms(const Vector<Op>& ops, uint32_t now_ms) {
    uint32_t best = 0u;

    for (uint32_t i = 0; i < ops.size(); i++) {
        const uint32_t t = op_next_wakeup_ms(ops[i]);

        if (t <= now_ms) {
            continue;
        }

        if (best == 0u || t < best) {
            best = t;
        }
    }

    return best;
}

bool DnsClient::try_get_next_wakeup_ms(uint32_t now_ms, uint32_t& out_ms) const {
    if (m_ops.size() == 0) {
        return false;
    }

    if (m_next_wakeup_ms != 0u && m_next_wakeup_ms > now_ms) {
        out_ms = m_next_wakeup_ms;
        return true;
    }

    const uint32_t best = recompute_next_wakeup_ms(m_ops, now_ms);
    if (best == 0u) {
        return false;
    }

    out_ms = best;
    return true;
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

    op.src_port = alloc_src_port(now_ms);

    op.dst_mac = Mac{};

    op.name_len = req.name_len;
    memcpy(op.name, req.name, req.name_len);

    op.state = 0u;

    if (!m_ops.push_back(op)) {
        return false;
    }

    (void)m_key_to_index.put(op.key, m_ops.size() - 1u);

    const uint32_t wake = op_next_wakeup_ms(op);
    if (m_next_wakeup_ms == 0u || wake < m_next_wakeup_ms) {
        m_next_wakeup_ms = wake;
    }

    return true;
}

void DnsClient::complete_op(uint32_t op_index, uint32_t ip_be, uint8_t ok, uint32_t now_ms) {
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

    if (m_next_wakeup_ms != 0u && m_next_wakeup_ms <= now_ms) {
        m_next_wakeup_ms = recompute_next_wakeup_ms(m_ops, now_ms);
    }
}

bool DnsClient::try_send_query(Op& op, uint32_t now_ms) {
    uint8_t dns[256];
    uint32_t dns_len = 0;

    if (!dns_wire::build_dns_a_query(op.txid, op.name, op.name_len, dns, (uint32_t)sizeof(dns), dns_len)) {
        return false;
    }

    (void)now_ms;
    return m_udp.send_to(op.dst_mac, op.dst_ip_be, op.src_port, kDnsPort, dns, dns_len, now_ms);
}

void DnsClient::step(uint32_t now_ms) {
    for (uint32_t i = 0; i < m_ops.size();) {
        Op& op = m_ops[i];

        if (now_ms >= op.deadline_ms) {
            complete_op(i, 0u, 0u, now_ms);
            continue;
        }

        if (op.state == 0u) {
            Mac mac{};
            if (m_arp.cache().lookup(op.next_hop_ip_be, mac, now_ms) && !mac_is_zero(mac)) {
                op.dst_mac = mac;
                op.state = 1u;
                op.next_tx_ms = now_ms;

                const uint32_t wake = op_next_wakeup_ms(op);
                if (m_next_wakeup_ms == 0u || wake < m_next_wakeup_ms) {
                    m_next_wakeup_ms = wake;
                }

                i++;
                continue;
            }

            if (now_ms >= op.next_arp_tx_ms) {
                (void)m_arp.request(op.next_hop_ip_be);
                op.next_arp_tx_ms = now_ms + 200u;

                const uint32_t wake = op_next_wakeup_ms(op);
                if (m_next_wakeup_ms == 0u || wake < m_next_wakeup_ms) {
                    m_next_wakeup_ms = wake;
                }
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

            {
                const uint32_t wake = op_next_wakeup_ms(op);
                if (m_next_wakeup_ms == 0u || wake < m_next_wakeup_ms) {
                    m_next_wakeup_ms = wake;
                }
            }

            i++;
            continue;
        }

        i++;
    }

    if (m_next_wakeup_ms != 0u && m_next_wakeup_ms <= now_ms) {
        m_next_wakeup_ms = recompute_next_wakeup_ms(m_ops, now_ms);
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

    for (uint32_t i = 0; i < self->m_ops.size(); i++) {
        Op& op = self->m_ops[i];

        if (op.dst_ip_be != ip->src) {
            continue;
        }

        if (op.txid != txid) {
            continue;
        }

        if (op.src_port != dst_port) {
            continue;
        }

        uint32_t ip_be = 0;
        if (dns_wire::parse_dns_a_response(txid, payload, payload_len, ip_be)) {
            self->complete_op(i, ip_be, 1u, now_ms);
            return true;
        }

        self->complete_op(i, 0u, 0u, now_ms);
        return true;
    }

    return false;
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
