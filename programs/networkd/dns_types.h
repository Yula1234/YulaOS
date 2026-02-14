#ifndef YOS_NETD_DNS_TYPES_H
#define YOS_NETD_DNS_TYPES_H

#include "net_proto.h"

#include <stdint.h>

namespace netd {

struct DnsConfig {
    uint32_t ip_be;
    uint32_t gw_be;
    uint32_t dns_ip_be;
};

struct ResolveRequest {
    uint8_t name_len;
    char name[127];

    uint32_t tag;
    uint32_t client_token;

    uint32_t timeout_ms;
};

struct ResolveResult {
    uint32_t ip_be;
    uint8_t ok;

    uint32_t tag;
    uint32_t client_token;
};

}

#endif
