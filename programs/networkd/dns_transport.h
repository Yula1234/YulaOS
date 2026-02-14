#ifndef YOS_NETD_DNS_TRANSPORT_H
#define YOS_NETD_DNS_TRANSPORT_H

#include "dns_types.h"
#include "net_proto.h"

namespace netd {
class NetDev;
}

namespace netd::dns_transport {

uint16_t alloc_src_port(uint32_t now_ms);

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
);

}

#endif
