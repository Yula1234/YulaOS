#include "dns_transport.h"

#include "dns_wire.h"
#include "net_mac.h"
#include "netdev.h"

#include <yula.h>

namespace netd::dns_transport {

namespace {

static constexpr uint16_t kDnsPort = 53u;

static uint16_t rand16(uint32_t now_ms) {
    const uint32_t t = now_ms;
    return (uint16_t)((t & 0xFFFFu) ^ (t >> 16));
}

}

uint16_t alloc_src_port(uint32_t now_ms) {
    return (uint16_t)(40000u + (rand16(now_ms) % 20000u));
}

bool send_a_query(
    netd::NetDev& dev,
    const DnsConfig& cfg,
    const Mac& dst_mac,
    uint32_t dst_ip_be,
    uint16_t src_port,
    uint16_t txid,
    const char* name,
    uint8_t name_len,
    uint32_t now_ms
) {
    uint8_t dns[256];
    uint32_t dns_len = 0;

    if (!dns_wire::build_dns_a_query(txid, name, name_len, dns, (uint32_t)sizeof(dns), dns_len)) {
        return false;
    }

    uint8_t buf[1600];

    const uint32_t udp_len = (uint32_t)sizeof(UdpHdr) + dns_len;
    const uint32_t frame_len = (uint32_t)(sizeof(EthHdr) + sizeof(Ipv4Hdr) + udp_len);

    if (frame_len > sizeof(buf)) {
        return false;
    }

    EthHdr* eth = (EthHdr*)buf;
    Ipv4Hdr* ip = (Ipv4Hdr*)(buf + sizeof(EthHdr));
    UdpHdr* udp = (UdpHdr*)((uint8_t*)ip + sizeof(Ipv4Hdr));

    mac_to_bytes(dst_mac, eth->dst);
    mac_to_bytes(dev.mac(), eth->src);
    eth->ethertype = htons(ETHERTYPE_IPV4);

    ip->ver_ihl = 0x45u;
    ip->tos = 0;
    ip->total_len = htons((uint16_t)(sizeof(Ipv4Hdr) + udp_len));
    ip->id = htons((uint16_t)(now_ms & 0xFFFFu));
    ip->frag_off = 0;
    ip->ttl = 64;
    ip->proto = IP_PROTO_UDP;
    ip->hdr_checksum = 0;
    ip->src = cfg.ip_be;
    ip->dst = dst_ip_be;
    ip->hdr_checksum = htons(checksum16(ip, (uint32_t)sizeof(Ipv4Hdr)));

    udp->src_port = htons(src_port);
    udp->dst_port = htons(kDnsPort);
    udp->len = htons((uint16_t)udp_len);
    udp->checksum = 0;

    memcpy((uint8_t*)udp + sizeof(UdpHdr), dns, dns_len);

    return dev.write_frame(buf, frame_len) > 0;
}

}
