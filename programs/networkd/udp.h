#ifndef YOS_NETD_UDP_H
#define YOS_NETD_UDP_H

#include "net_dispatch.h"
#include "net_proto.h"

#include <stdint.h>

namespace netd {

class Arena;
class Ipv4;

class Udp {
public:
    using HandlerFn = bool (*) (
        void* ctx,
        const Ipv4Hdr* ip,
        uint16_t src_port,
        uint16_t dst_port,
        const uint8_t* payload,
        uint32_t payload_len,
        uint32_t now_ms
    );

    Udp(Arena& arena, Ipv4& ipv4);

    Udp(const Udp&) = delete;
    Udp& operator=(const Udp&) = delete;

    bool add_port_handler(uint16_t dst_port, void* ctx, HandlerFn fn);

    void set_default_handler(void* ctx, HandlerFn fn);

    bool send_to(
        const Mac& dst_mac,
        uint32_t dst_ip_be,
        uint16_t src_port,
        uint16_t dst_port,
        const uint8_t* payload,
        uint32_t payload_len,
        uint32_t now_ms
    );

    static bool ip_proto_udp_handler(
        void* ctx,
        const EthHdr* eth,
        const Ipv4Hdr* ip,
        const uint8_t* payload,
        uint32_t payload_len,
        uint32_t now_ms
    );

private:
    struct Entry {
        void* ctx;
        HandlerFn fn;
    };

    using Table = detail::DispatchTable<uint16_t, Entry, 8u>;

    bool handle_udp(
        const Ipv4Hdr* ip,
        uint16_t src_port,
        uint16_t dst_port,
        const uint8_t* payload,
        uint32_t payload_len,
        uint32_t now_ms
    );

    Ipv4& m_ipv4;

    Table m_port_tab;

    void* m_default_ctx;
    HandlerFn m_default_fn;
};

}

#endif
