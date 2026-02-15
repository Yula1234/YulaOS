#include "udp.h"

#include "arena.h"
#include "ipv4.h"
#include "net_packet_builder.h"

#include <yula.h>

namespace netd {

Udp::Udp(Arena& arena, Ipv4& ipv4)
    : m_ipv4(ipv4),
      m_port_tab(arena),
      m_default_ctx(nullptr),
      m_default_fn(nullptr) {
}

bool Udp::add_port_handler(uint16_t dst_port, void* ctx, HandlerFn fn) {
    Entry e{};
    e.ctx = ctx;
    e.fn = fn;

    return m_port_tab.put(dst_port, e);
}

void Udp::set_default_handler(void* ctx, HandlerFn fn) {
    m_default_ctx = ctx;
    m_default_fn = fn;
}

bool Udp::send_to(
    const Mac& dst_mac,
    uint32_t dst_ip_be,
    uint16_t src_port,
    uint16_t dst_port,
    const uint8_t* payload,
    uint32_t payload_len,
    uint32_t now_ms
) {
    PacketBuilder pb;

    const uint32_t udp_len = (uint32_t)sizeof(UdpHdr) + payload_len;
    UdpHdr* udp = (UdpHdr*)pb.append((uint32_t)sizeof(UdpHdr));
    if (!udp) {
        return false;
    }

    udp->src_port = htons(src_port);
    udp->dst_port = htons(dst_port);
    udp->len = htons((uint16_t)udp_len);
    udp->checksum = 0;

    if (!pb.append_copy(payload, payload_len)) {
        return false;
    }

    const uint16_t id_be = htons((uint16_t)(now_ms & 0xFFFFu));
    return m_ipv4.send_packet(pb, dst_mac, dst_ip_be, IP_PROTO_UDP, id_be);
}

bool Udp::ip_proto_udp_handler(
    void* ctx,
    const EthHdr* eth,
    const Ipv4Hdr* ip,
    const uint8_t* payload,
    uint32_t payload_len,
    uint32_t now_ms
) {
    (void)eth;

    Udp* self = (Udp*)ctx;
    if (!self || !ip || !payload) {
        return false;
    }

    if (payload_len < (uint32_t)sizeof(UdpHdr)) {
        return false;
    }

    const UdpHdr* udp = (const UdpHdr*)payload;

    const uint32_t udp_len = (uint32_t)ntohs(udp->len);
    if (udp_len < sizeof(UdpHdr) || udp_len > payload_len) {
        return false;
    }

    const uint16_t src_port = ntohs(udp->src_port);
    const uint16_t dst_port = ntohs(udp->dst_port);

    const uint8_t* udp_payload = payload + sizeof(UdpHdr);
    const uint32_t udp_payload_len = udp_len - (uint32_t)sizeof(UdpHdr);

    return self->handle_udp(ip, src_port, dst_port, udp_payload, udp_payload_len, now_ms);
}

bool Udp::handle_udp(
    const Ipv4Hdr* ip,
    uint16_t src_port,
    uint16_t dst_port,
    const uint8_t* payload,
    uint32_t payload_len,
    uint32_t now_ms
) {
    Entry e{};
    if (m_port_tab.get(dst_port, e) && e.fn) {
        return e.fn(e.ctx, ip, src_port, dst_port, payload, payload_len, now_ms);
    }

    if (!m_default_fn) {
        return false;
    }

    return m_default_fn(m_default_ctx, ip, src_port, dst_port, payload, payload_len, now_ms);
}

}
