#include "netd_core_stack.h"

namespace netd {

uint32_t NetdCoreStack::ip_be(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    const uint32_t ip = ((uint32_t)a << 24) |
                        ((uint32_t)b << 16) |
                        ((uint32_t)c << 8) |
                        (uint32_t)d;

    return htonl(ip);
}

NetdCoreStack::NetdCoreStack(Arena& arena, NetDev& dev)
    : m_arena(arena),
      m_dev(dev),
      m_arp(),
      m_ip(),
      m_dns(),
      m_eth_dispatch() {
}

bool NetdCoreStack::init() {
    Arp* arp = m_arp.construct(m_arena, m_dev);
    Ipv4Icmp* ip = m_ip.construct(m_arena, m_dev, *arp);
    DnsClient* dns = m_dns.construct(m_arena, m_dev, *arp);
    EthertypeDispatch* eth = m_eth_dispatch.construct(m_arena);

    const Mac mac = m_dev.mac();

    ArpConfig arp_cfg{};
    arp_cfg.ip_be = default_ip_be();
    arp_cfg.mac = mac;
    arp->set_config(arp_cfg);

    IpConfig ip_cfg{};
    ip_cfg.ip_be = arp_cfg.ip_be;
    ip_cfg.mask_be = default_mask_be();
    ip_cfg.gw_be = default_gw_be();
    ip->set_config(ip_cfg);

    DnsConfig dns_cfg{};
    dns_cfg.ip_be = ip_cfg.ip_be;
    dns_cfg.gw_be = ip_cfg.gw_be;
    dns_cfg.dns_ip_be = default_dns_be();
    dns->set_config(dns_cfg);

    (void)ip->add_proto_handler(IP_PROTO_UDP, dns, &DnsClient::udp_proto_handler);

    (void)eth->reserve(8u);
    (void)eth->add(ETHERTYPE_ARP, arp, &NetdCoreStack::handle_arp);
    (void)eth->add(ETHERTYPE_IPV4, ip, &NetdCoreStack::handle_ipv4);

    return true;
}

void NetdCoreStack::handle_arp(void* ctx, const uint8_t* frame, uint32_t len, uint32_t now_ms) {
    Arp* arp = (Arp*)ctx;
    if (!arp) {
        return;
    }

    (void)arp->handle_frame(frame, len, now_ms);
}

void NetdCoreStack::handle_ipv4(void* ctx, const uint8_t* frame, uint32_t len, uint32_t now_ms) {
    Ipv4Icmp* ip = (Ipv4Icmp*)ctx;
    if (!ip) {
        return;
    }

    (void)ip->handle_frame(frame, len, now_ms);
}

void NetdCoreStack::step(uint32_t now_ms) {
    m_ip->step(now_ms);
    m_dns->step(now_ms);
}

bool NetdCoreStack::handle_frame(const uint8_t* frame, uint32_t len, uint32_t now_ms) {
    if (!frame || len < (uint32_t)sizeof(EthHdr)) {
        return false;
    }

    const EthHdr* eth = (const EthHdr*)frame;
    const uint16_t et = ntohs(eth->ethertype);

    return m_eth_dispatch->dispatch(et, frame, len, now_ms);
}

bool NetdCoreStack::submit_ping(const Ipv4Icmp::PingRequest& req, uint32_t now_ms) {
    return m_ip->submit_ping(req, now_ms);
}

bool NetdCoreStack::poll_ping_result(Ipv4Icmp::PingResult& out) {
    return m_ip->poll_result(out);
}

bool NetdCoreStack::submit_resolve(const ResolveRequest& req, uint32_t now_ms) {
    return m_dns->submit_resolve(req, now_ms);
}

bool NetdCoreStack::poll_resolve_result(ResolveResult& out) {
    return m_dns->poll_result(out);
}

bool NetdCoreStack::try_get_next_wakeup_ms(uint32_t now_ms, uint32_t& out_ms) const {
    uint32_t best = 0u;

    uint32_t t = 0u;
    if (m_ip->try_get_next_wakeup_ms(now_ms, t)) {
        best = t;
    }

    if (m_dns->try_get_next_wakeup_ms(now_ms, t)) {
        if (best == 0u || t < best) {
            best = t;
        }
    }

    if (best == 0u) {
        return false;
    }

    out_ms = best;
    return true;
}

bool NetdCoreStack::resolve_gateway(uint32_t gw_ip_be, Mac& out_mac, uint32_t timeout_ms) {
    return m_arp->resolve(gw_ip_be, out_mac, timeout_ms);
}

Mac NetdCoreStack::mac() const {
    return m_dev.mac();
}

uint32_t NetdCoreStack::default_ip_be() const {
    return ip_be(10, 0, 2, 15);
}

uint32_t NetdCoreStack::default_mask_be() const {
    return ip_be(255, 255, 255, 0);
}

uint32_t NetdCoreStack::default_gw_be() const {
    return ip_be(10, 0, 2, 2);
}

uint32_t NetdCoreStack::default_dns_be() const {
    return ip_be(8, 8, 8, 8);
}

}
