#ifndef YOS_NETD_CONFIG_H
#define YOS_NETD_CONFIG_H

#include "net_proto.h"

#include <stdint.h>

namespace netd {

struct NetdConfig {
    uint32_t ip_be;
    uint32_t mask_be;
    uint32_t gw_be;
    uint32_t dns_ip_be;
};

static inline uint32_t make_ipv4_be(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    const uint32_t ip = ((uint32_t)a << 24) |
                        ((uint32_t)b << 16) |
                        ((uint32_t)c << 8) |
                        (uint32_t)d;

    return htonl(ip);
}

static inline NetdConfig default_netd_config() {
    NetdConfig cfg{};
    cfg.ip_be = make_ipv4_be(10, 0, 2, 15);
    cfg.mask_be = make_ipv4_be(255, 255, 255, 0);
    cfg.gw_be = make_ipv4_be(10, 0, 2, 2);
    cfg.dns_ip_be = make_ipv4_be(10, 0, 2, 3);
    return cfg;
}

}

#endif
