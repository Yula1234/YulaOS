#include "ipv4_icmp.h"

#include <yula.h>

namespace netd {

namespace {

struct ParsedIpv4 {
    const netd::EthHdr* eth;
    const netd::Ipv4Hdr* ip;
    const uint8_t* payload;
    uint32_t payload_len;
};

static bool parse_ipv4_frame(const uint8_t* frame, uint32_t len, ParsedIpv4& out) {
    if (!frame || len < (uint32_t)(sizeof(netd::EthHdr) + sizeof(netd::Ipv4Hdr))) {
        return false;
    }

    const netd::EthHdr* eth = (const netd::EthHdr*)frame;
    if (netd::ntohs(eth->ethertype) != netd::ETHERTYPE_IPV4) {
        return false;
    }

    const netd::Ipv4Hdr* ip = (const netd::Ipv4Hdr*)(frame + sizeof(netd::EthHdr));
    const uint32_t ihl = (uint32_t)(ip->ver_ihl & 0x0Fu) * 4u;
    if (ihl < sizeof(netd::Ipv4Hdr)) {
        return false;
    }

    if (len < (uint32_t)sizeof(netd::EthHdr) + ihl) {
        return false;
    }

    const uint16_t total_len = netd::ntohs(ip->total_len);
    if (total_len < ihl) {
        return false;
    }

    const uint32_t payload_len = (uint32_t)total_len - ihl;
    if (len < (uint32_t)sizeof(netd::EthHdr) + ihl + payload_len) {
        return false;
    }

    out.eth = eth;
    out.ip = ip;
    out.payload = (const uint8_t*)ip + ihl;
    out.payload_len = payload_len;
    return true;
}

static bool match_icmp_echo_reply(const ParsedIpv4& p, uint16_t ident_be, uint16_t seq_be) {
    if (!p.ip || p.ip->proto != netd::IP_PROTO_ICMP) {
        return false;
    }

    if (!p.payload || p.payload_len < (uint32_t)sizeof(netd::IcmpHdr)) {
        return false;
    }

    const netd::IcmpHdr* icmp = (const netd::IcmpHdr*)p.payload;
    if (icmp->type != netd::ICMP_ECHO_REPLY) {
        return false;
    }

    return icmp->ident == ident_be && icmp->seq == seq_be;
}

}

Ipv4Icmp::Ipv4Icmp(Arena& arena, NetDev& dev, Arp& arp)
    : m_dev(dev),
      m_arp(arp),
      m_cfg{},
      m_proto_dispatch(arena),
      m_ops(arena),
      m_key_to_index(arena),
      m_results(arena) {
    (void)m_proto_dispatch.reserve(4u);
    (void)m_proto_dispatch.add(IP_PROTO_ICMP, this, &Ipv4Icmp::proto_icmp_handler);
    (void)m_ops.reserve(32u);
    (void)m_results.reserve(32u);
    (void)m_key_to_index.reserve(64u);
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

void Ipv4Icmp::set_config(const IpConfig& cfg) {
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

uint32_t Ipv4Icmp::next_hop_ip(uint32_t dst_ip_be) const {
    const uint32_t ip = ntohl(m_cfg.ip_be);
    const uint32_t mask = ntohl(m_cfg.mask_be);
    const uint32_t dst = ntohl(dst_ip_be);

    if (((ip ^ dst) & mask) == 0u) {
        return dst_ip_be;
    }

    return m_cfg.gw_be;
}

bool Ipv4Icmp::send_ipv4(const Mac& dst_mac, uint32_t dst_ip_be, uint8_t proto, const uint8_t* payload, uint32_t payload_len) {
    uint8_t buf[1600];
    const uint32_t frame_len = (uint32_t)(sizeof(EthHdr) + sizeof(Ipv4Hdr) + payload_len);

    if (frame_len > sizeof(buf)) {
        return false;
    }

    EthHdr* eth = (EthHdr*)buf;
    Ipv4Hdr* ip = (Ipv4Hdr*)(buf + sizeof(EthHdr));

    mac_to_bytes(dst_mac, eth->dst);
    mac_to_bytes(m_dev.mac(), eth->src);
    eth->ethertype = htons(ETHERTYPE_IPV4);

    ip->ver_ihl = 0x45u;
    ip->tos = 0;
    ip->total_len = htons((uint16_t)(sizeof(Ipv4Hdr) + payload_len));
    ip->id = 0;
    ip->frag_off = 0;
    ip->ttl = 64;
    ip->proto = proto;
    ip->hdr_checksum = 0;
    ip->src = m_cfg.ip_be;
    ip->dst = dst_ip_be;
    ip->hdr_checksum = htons(checksum16(ip, sizeof(Ipv4Hdr)));

    if (payload_len > 0) {
        memcpy((uint8_t*)ip + sizeof(Ipv4Hdr), payload, payload_len);
    }

    return m_dev.write_frame(buf, frame_len) > 0;
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

    uint8_t out[1600];
    const uint32_t icmp_len = payload_len;
    if ((uint32_t)(sizeof(Ipv4Hdr) + icmp_len) > (uint32_t)(sizeof(out) - sizeof(EthHdr))) {
        return false;
    }

    IcmpHdr* rep = (IcmpHdr*)(out + sizeof(EthHdr) + sizeof(Ipv4Hdr));

    memcpy(rep, payload, icmp_len);

    rep->type = ICMP_ECHO_REPLY;
    rep->checksum = 0;
    rep->checksum = htons(checksum16(rep, icmp_len));

    const Mac dst_mac = mac_from_bytes(((const EthHdr*)((const uint8_t*)eth))->src);
    return send_ipv4(dst_mac, ip->src, IP_PROTO_ICMP, (const uint8_t*)rep, icmp_len);
}

bool Ipv4Icmp::handle_frame(const uint8_t* frame, uint32_t len, uint32_t now_ms) {
    ParsedIpv4 p{};
    if (!parse_ipv4_frame(frame, len, p)) {
        return false;
    }

    if (p.ip->dst != m_cfg.ip_be) {
        return true;
    }

    if (m_proto_dispatch.dispatch(p.ip->proto, p.eth, p.ip, p.payload, p.payload_len, now_ms)) {
        return true;
    }

    return true;
}

bool Ipv4Icmp::handle_proto_icmp(
    const EthHdr* eth,
    const Ipv4Hdr* ip,
    const uint8_t* payload,
    uint32_t payload_len,
    uint32_t now_ms
) {
    if (payload && payload_len >= (uint32_t)sizeof(IcmpHdr)) {
        const IcmpHdr* icmp = (const IcmpHdr*)payload;
        if (icmp->type == ICMP_ECHO_REPLY) {
            const uint32_t key = make_key(icmp->ident, icmp->seq);

            uint32_t idx = 0;
            if (m_key_to_index.get(key, idx)) {
                if (idx < m_ops.size()) {
                    complete_op(idx, now_ms, 1u);
                }
            }

            return true;
        }
    }

    return handle_icmp(eth, ip, payload, payload_len);
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
    op.next_hop_ip_be = next_hop_ip(req.dst_ip_be);
    op.ident_be = req.ident_be;
    op.seq_be = req.seq_be;
    op.deadline_ms = now_ms + req.timeout_ms;
    op.sent_time_ms = 0;
    op.next_arp_tx_ms = now_ms;
    op.dst_mac = Mac{};
    op.state = 0u;

    if (!m_ops.push_back(op)) {
        return false;
    }

    const uint32_t idx = m_ops.size() - 1u;
    if (!m_key_to_index.put(op.key, idx)) {
        m_ops.erase_unordered(idx);
        return false;
    }

    return true;
}

void Ipv4Icmp::complete_op(uint32_t op_index, uint32_t now_ms, uint8_t ok) {
    if (op_index >= m_ops.size()) {
        return;
    }

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

    (void)m_key_to_index.erase(op.key);

    const uint32_t last = m_ops.size() - 1u;
    if (op_index != last) {
        const PingOp moved = m_ops[last];
        m_ops[op_index] = moved;
        (void)m_key_to_index.put(moved.key, op_index);
    }

    m_ops.erase_unordered(last);
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

                if (!send_ipv4(op.dst_mac, op.dst_ip_be, IP_PROTO_ICMP, payload, (uint32_t)sizeof(payload))) {
                    complete_op(i, now_ms, 0u);
                    continue;
                }

                op.sent_time_ms = now_ms;
                op.state = 1u;

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

        i++;
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
