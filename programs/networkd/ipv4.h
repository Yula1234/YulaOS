#ifndef YOS_NETD_IPV4_H
#define YOS_NETD_IPV4_H

#include "net_proto.h"
#include "net_dispatch.h"

#include <stdint.h>

namespace netd {

class Arena;
class NetDev;

struct IpConfig {
    uint32_t ip_be;
    uint32_t mask_be;
    uint32_t gw_be;
};

class Ipv4 {
public:
    Ipv4(Arena& arena, NetDev& dev);

    Ipv4(const Ipv4&) = delete;
    Ipv4& operator=(const Ipv4&) = delete;

    void set_config(const IpConfig& cfg);

    uint32_t src_ip_be() const;

    bool handle_frame(const uint8_t* frame, uint32_t len, uint32_t now_ms);

    bool add_proto_handler(uint8_t proto, void* ctx, IpProtoDispatch::HandlerFn fn);

    uint32_t next_hop_ip(uint32_t dst_ip_be) const;

    bool send_packet(
        const Mac& dst_mac,
        uint32_t dst_ip_be,
        uint8_t proto,
        const uint8_t* payload,
        uint32_t payload_len,
        uint16_t id_be
    );

private:
    NetDev& m_dev;
    IpConfig m_cfg;

    IpProtoDispatch m_proto_dispatch;
};

}

#endif
