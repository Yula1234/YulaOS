#include "ipv4_icmp.h"

#include "arena.h"
#include "arp.h"
#include "net_mac.h"
#include "net_packet_builder.h"

#include <yula.h>

namespace netd {

Ipv4Icmp::Ipv4Icmp(Arena& arena, Ipv4& ipv4, Arp& arp)
    : m_ipv4(ipv4),
      m_arp(arp),
      m_ops(arena),
      m_key_to_index(arena),
      m_results(arena),
      m_next_wakeup_ms(0u) {
    (void)m_ops.reserve(32u);
    (void)m_results.reserve(32u);
}

uint32_t Ipv4Icmp::op_next_wakeup_ms(const PingOp& op) {
    uint32_t t = op.deadline_ms;

    if (op.state == 0u) {
        if (op.next_arp_tx_ms < t) {
            t = op.next_arp_tx_ms;
        }
    }

    return t;
}

uint32_t Ipv4Icmp::recompute_next_wakeup_ms(const Vector<PingOp>& ops, uint32_t now_ms) {
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

bool Ipv4Icmp::try_get_next_wakeup_ms(uint32_t now_ms, uint32_t& out_ms) const {
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

bool Ipv4Icmp::proto_icmp_handler(
    void* ctx,
    const EthHdr* eth,
    const Ipv4Hdr* ip,
    const uint8_t* payload,
    uint32_t payload_len,
    uint32_t now_ms
) {
    Ipv4Icmp* self = (Ipv4Icmp*)ctx;
    if (!self) {
        return false;
    }

    return self->handle_proto_icmp(eth, ip, payload, payload_len, now_ms);
}

bool Ipv4Icmp::handle_icmp(const EthHdr* eth, const Ipv4Hdr* ip, const uint8_t* payload, uint32_t payload_len) {
    (void)eth;

    if (!payload || payload_len < (uint32_t)sizeof(IcmpHdr)) {
        return false;
    }

    const IcmpHdr* icmp = (const IcmpHdr*)payload;
    if (icmp->type != ICMP_ECHO_REQUEST || icmp->code != 0u) {
        return true;
    }

    const uint32_t icmp_len = payload_len;

    PacketBuilder pb;
    IcmpHdr* rep = (IcmpHdr*)pb.append(icmp_len);
    if (!rep) {
        return false;
    }

    memcpy(rep, payload, icmp_len);

    rep->type = ICMP_ECHO_REPLY;
    rep->checksum = 0;
    rep->checksum = htons(checksum16(rep, icmp_len));

    const Mac dst_mac = mac_from_bytes(((const EthHdr*)((const uint8_t*)eth))->src);
    return m_ipv4.send_packet(pb, dst_mac, ip->src, IP_PROTO_ICMP, 0);
}

bool Ipv4Icmp::handle_proto_icmp(
    const EthHdr* eth,
    const Ipv4Hdr* ip,
    const uint8_t* payload,
    uint32_t payload_len,
    uint32_t now_ms
) {
    if (!payload || payload_len < (uint32_t)sizeof(IcmpHdr)) {
        return false;
    }

    const IcmpHdr* icmp = (const IcmpHdr*)payload;
    if (icmp->type != ICMP_ECHO_REPLY) {
        return handle_icmp(eth, ip, payload, payload_len);
    }

    const uint32_t key = make_key(icmp->ident, icmp->seq);

    uint32_t idx = 0;
    if (!m_key_to_index.get(key, idx)) {
        return true;
    }

    if (idx >= m_ops.size()) {
        return true;
    }

    const PingOp& op = m_ops[idx];
    if (op.key != key || op.dst_ip_be != ip->src) {
        return true;
    }

    complete_op(idx, now_ms, 1u);

    return true;
}

uint32_t Ipv4Icmp::make_key(uint16_t ident_be, uint16_t seq_be) {
    return ((uint32_t)ident_be << 16) | (uint32_t)seq_be;
}

bool Ipv4Icmp::submit_ping(const PingRequest& req, uint32_t now_ms) {
    PingOp op{};
    op.key = make_key(req.ident_be, req.seq_be);
    op.tag = req.tag;
    op.client_token = req.client_token;
    op.dst_ip_be = req.dst_ip_be;
    op.next_hop_ip_be = m_ipv4.next_hop_ip(req.dst_ip_be);
    op.ident_be = req.ident_be;
    op.seq_be = req.seq_be;
    op.deadline_ms = now_ms + req.timeout_ms;
    op.sent_time_ms = 0;
    op.next_arp_tx_ms = now_ms;
    op.dst_mac = Mac{};
    op.state = 0u;

    {
        uint32_t existing = 0;
        if (m_key_to_index.get(op.key, existing)) {
            return false;
        }
    }

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

void Ipv4Icmp::complete_op(uint32_t op_index, uint32_t now_ms, uint8_t ok) {
    if (op_index >= m_ops.size()) {
        return;
    }

    const uint32_t last = m_ops.size() - 1u;
    const uint32_t removed_key = m_ops[op_index].key;
    const uint32_t moved_key = (op_index != last) ? m_ops[last].key : 0u;

    PingOp& op = m_ops[op_index];

    PingResult r{};
    r.tag = op.tag;
    r.client_token = op.client_token;
    r.dst_ip_be = op.dst_ip_be;
    r.ident_be = op.ident_be;
    r.seq_be = op.seq_be;
    r.ok = ok;

    if (ok && op.sent_time_ms != 0u && now_ms >= op.sent_time_ms) {
        r.rtt_ms = now_ms - op.sent_time_ms;
    } else {
        r.rtt_ms = 0u;
    }

    (void)m_results.push_back(r);

    (void)m_key_to_index.erase(removed_key);

    m_ops.erase_unordered(op_index);

    if (moved_key != 0u) {
        (void)m_key_to_index.put(moved_key, op_index);
    }

    if (m_next_wakeup_ms != 0u && m_next_wakeup_ms <= now_ms) {
        m_next_wakeup_ms = recompute_next_wakeup_ms(m_ops, now_ms);
    }
}

void Ipv4Icmp::step(uint32_t now_ms) {
    uint32_t i = 0;
    while (i < m_ops.size()) {
        PingOp& op = m_ops[i];

        if (now_ms >= op.deadline_ms) {
            complete_op(i, now_ms, 0u);
            continue;
        }

        if (op.state == 0u) {
            m_arp.cache().lookup(op.next_hop_ip_be, op.dst_mac, now_ms);
            if (!mac_is_zero(op.dst_mac)) {
                uint8_t payload[64];
                IcmpHdr* icmp = (IcmpHdr*)payload;

                icmp->type = ICMP_ECHO_REQUEST;
                icmp->code = 0;
                icmp->checksum = 0;
                icmp->ident = op.ident_be;
                icmp->seq = op.seq_be;

                for (uint32_t j = (uint32_t)sizeof(IcmpHdr); j < sizeof(payload); j++) {
                    payload[j] = (uint8_t)j;
                }

                icmp->checksum = htons(checksum16(payload, (uint32_t)sizeof(payload)));

                if (!m_ipv4.send_packet(op.dst_mac, op.dst_ip_be, IP_PROTO_ICMP, payload, (uint32_t)sizeof(payload), 0)) {
                    complete_op(i, now_ms, 0u);
                    continue;
                }

                op.sent_time_ms = now_ms;
                op.state = 1u;

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

        i++;
    }

    if (m_next_wakeup_ms != 0u && m_next_wakeup_ms <= now_ms) {
        m_next_wakeup_ms = recompute_next_wakeup_ms(m_ops, now_ms);
    }
}

bool Ipv4Icmp::poll_result(PingResult& out) {
    if (m_results.size() == 0) {
        return false;
    }

    out = m_results[0];
    m_results.erase_unordered(0);
    return true;
}

}
