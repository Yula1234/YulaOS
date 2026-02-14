#ifndef YOS_NETD_PROTO_H
#define YOS_NETD_PROTO_H

#include <stdint.h>

namespace netd {

static inline uint16_t bswap16(uint16_t v) {
    return (uint16_t)((v << 8) | (v >> 8));
}

static inline uint32_t bswap32(uint32_t v) {
    return ((v & 0x000000FFu) << 24) |
           ((v & 0x0000FF00u) << 8) |
           ((v & 0x00FF0000u) >> 8) |
           ((v & 0xFF000000u) >> 24);
}

static inline uint16_t htons(uint16_t v) {
    return bswap16(v);
}

static inline uint16_t ntohs(uint16_t v) {
    return bswap16(v);
}

static inline uint32_t htonl(uint32_t v) {
    return bswap32(v);
}

static inline uint32_t ntohl(uint32_t v) {
    return bswap32(v);
}

struct Mac {
    uint8_t b[6];

    bool operator==(const Mac& o) const {
        for (int i = 0; i < 6; i++) {
            if (b[i] != o.b[i]) {
                return false;
            }
        }
        return true;
    }
};

static inline bool mac_is_zero(const Mac& m) {
    for (int i = 0; i < 6; i++) {
        if (m.b[i] != 0u) {
            return false;
        }
    }
    return true;
}

static inline Mac mac_broadcast() {
    Mac m{};

    for (int i = 0; i < 6; i++) {
        m.b[i] = 0xFFu;
    }

    return m;
}

struct __attribute__((packed)) EthHdr {
    uint8_t dst[6];
    uint8_t src[6];
    uint16_t ethertype;
};

static constexpr uint16_t ETHERTYPE_ARP = 0x0806u;
static constexpr uint16_t ETHERTYPE_IPV4 = 0x0800u;

struct __attribute__((packed)) ArpHdr {
    uint16_t htype;
    uint16_t ptype;
    uint8_t hlen;
    uint8_t plen;
    uint16_t oper;
    uint8_t sha[6];
    uint32_t spa;
    uint8_t tha[6];
    uint32_t tpa;
};

static constexpr uint16_t ARP_HTYPE_ETH = 1u;
static constexpr uint16_t ARP_OPER_REQUEST = 1u;
static constexpr uint16_t ARP_OPER_REPLY = 2u;

struct __attribute__((packed)) Ipv4Hdr {
    uint8_t ver_ihl;
    uint8_t tos;
    uint16_t total_len;
    uint16_t id;
    uint16_t frag_off;
    uint8_t ttl;
    uint8_t proto;
    uint16_t hdr_checksum;
    uint32_t src;
    uint32_t dst;
};

static constexpr uint8_t IP_PROTO_ICMP = 1u;
static constexpr uint8_t IP_PROTO_UDP = 17u;

struct __attribute__((packed)) UdpHdr {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t len;
    uint16_t checksum;
};

struct __attribute__((packed)) IcmpHdr {
    uint8_t type;
    uint8_t code;
    uint16_t checksum;
    uint16_t ident;
    uint16_t seq;
};

static constexpr uint8_t ICMP_ECHO_REPLY = 0u;
static constexpr uint8_t ICMP_ECHO_REQUEST = 8u;

static inline uint16_t checksum16(const void* data, uint32_t len) {
    const uint8_t* p = (const uint8_t*)data;
    uint32_t sum = 0;

    while (len >= 2u) {
        uint16_t w = (uint16_t)p[0] << 8 | (uint16_t)p[1];
        sum += w;
        p += 2;
        len -= 2;
    }

    if (len != 0u) {
        uint16_t w = (uint16_t)p[0] << 8;
        sum += w;
    }

    while (sum >> 16u) {
        sum = (sum & 0xFFFFu) + (sum >> 16u);
    }

    return (uint16_t)~sum;
}

}

#endif
