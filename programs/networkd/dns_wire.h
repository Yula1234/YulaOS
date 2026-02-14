#ifndef YOS_NETD_DNS_WIRE_H
#define YOS_NETD_DNS_WIRE_H

#include <stdint.h>

namespace netd::dns_wire {

bool build_dns_a_query(
    uint16_t txid,
    const char* name,
    uint8_t name_len,
    uint8_t* out,
    uint32_t out_cap,
    uint32_t& out_len
);

bool parse_dns_a_response(uint16_t txid, const uint8_t* pkt, uint32_t len, uint32_t& out_ip_be);

}

#endif
