#include "ipv4.h"

#include "arena.h"
#include "net_packet_builder.h"
#include "netdev.h"
#include "net_mac.h"

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

}

Ipv4::Ipv4(Arena& arena, NetDev& dev)
    : m_dev(dev),
      m_cfg{},
      m_proto_dispatch(arena) {
    (void)m_proto_dispatch.reserve(8u);
}

void Ipv4::set_config(const IpConfig& cfg) {
    m_cfg = cfg;
}

uint32_t Ipv4::src_ip_be() const {
    return m_cfg.ip_be;
}

bool Ipv4::add_proto_handler(uint8_t proto, void* ctx, IpProtoDispatch::HandlerFn fn) {
    return m_proto_dispatch.add(proto, ctx, fn);
}

uint32_t Ipv4::next_hop_ip(uint32_t dst_ip_be) const {
    const uint32_t ip = ntohl(m_cfg.ip_be);
    const uint32_t mask = ntohl(m_cfg.mask_be);
    const uint32_t dst = ntohl(dst_ip_be);

    if (((ip ^ dst) & mask) == 0u) {
        return dst_ip_be;
    }

    return m_cfg.gw_be;
}

bool Ipv4::send_packet(
    const Mac& dst_mac,
    uint32_t dst_ip_be,
    uint8_t proto,
    const uint8_t* payload,
    uint32_t payload_len,
    uint16_t id_be
) {
    PacketBuilder pb;

    if (!pb.append_copy(payload, payload_len)) {
        return false;
    }

    return send_packet(pb, dst_mac, dst_ip_be, proto, id_be);
}

bool Ipv4::send_packet(
    PacketBuilder& pb,
    const Mac& dst_mac,
    uint32_t dst_ip_be,
    uint8_t proto,
    uint16_t id_be
) {
    const uint32_t payload_len = pb.size();

    Ipv4Hdr* ip = (Ipv4Hdr*)pb.prepend((uint32_t)sizeof(Ipv4Hdr));
    if (!ip) {
        return false;
    }

    EthHdr* eth = (EthHdr*)pb.prepend((uint32_t)sizeof(EthHdr));
    if (!eth) {
        return false;
    }

    mac_to_bytes(dst_mac, eth->dst);
    mac_to_bytes(m_dev.mac(), eth->src);
    eth->ethertype = htons(ETHERTYPE_IPV4);

    ip->ver_ihl = 0x45u;
    ip->tos = 0;
    ip->total_len = htons((uint16_t)((uint32_t)sizeof(Ipv4Hdr) + payload_len));
    ip->id = id_be;
    ip->frag_off = 0;
    ip->ttl = 64;
    ip->proto = proto;
    ip->hdr_checksum = 0;
    ip->src = m_cfg.ip_be;
    ip->dst = dst_ip_be;
    ip->hdr_checksum = htons(checksum16(ip, sizeof(Ipv4Hdr)));

    return m_dev.write_frame(pb.data(), pb.size()) > 0;
}

bool Ipv4::handle_frame(const uint8_t* frame, uint32_t len, uint32_t now_ms) {
    ParsedIpv4 p{};
    if (!parse_ipv4_frame(frame, len, p)) {
        return false;
    }

    if (p.ip->dst != m_cfg.ip_be) {
        return true;
    }

    (void)m_proto_dispatch.dispatch(p.ip->proto, p.eth, p.ip, p.payload, p.payload_len, now_ms);
    return true;
}

}
