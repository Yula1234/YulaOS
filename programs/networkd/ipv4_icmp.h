#ifndef YOS_NETD_IPV4_ICMP_H
#define YOS_NETD_IPV4_ICMP_H

#include "net_proto.h"
#include "netdev.h"
#include "arp.h"
#include "net_vec.h"
#include "net_dispatch.h"
#include "net_u32_map.h"

#include <stdint.h>

namespace netd {

struct IpConfig {
    uint32_t ip_be;
    uint32_t mask_be;
    uint32_t gw_be;
};

class Ipv4Icmp {
public:
    Ipv4Icmp(Arena& arena, NetDev& dev, Arp& arp);

    void set_config(const IpConfig& cfg);

    bool handle_frame(const uint8_t* frame, uint32_t len, uint32_t now_ms);

    struct PingRequest {
        uint32_t dst_ip_be;
        uint16_t ident_be;
        uint16_t seq_be;
        uint32_t timeout_ms;
        uint32_t tag;
        uint32_t client_token;
    };

    struct PingResult {
        uint32_t tag;
        uint32_t client_token;
        uint32_t dst_ip_be;
        uint16_t ident_be;
        uint16_t seq_be;
        uint32_t rtt_ms;
        uint8_t ok;
    };

    bool submit_ping(const PingRequest& req, uint32_t now_ms);
    void step(uint32_t now_ms);
    bool poll_result(PingResult& out);

    bool try_get_next_wakeup_ms(uint32_t now_ms, uint32_t& out_ms) const;

    bool add_proto_handler(uint8_t proto, void* ctx, IpProtoDispatch::HandlerFn fn);

private:
    bool send_ipv4(const Mac& dst_mac, uint32_t dst_ip_be, uint8_t proto, const uint8_t* payload, uint32_t payload_len);
    bool handle_icmp(const EthHdr* eth, const Ipv4Hdr* ip, const uint8_t* payload, uint32_t payload_len);

    bool handle_proto_icmp(const EthHdr* eth, const Ipv4Hdr* ip, const uint8_t* payload, uint32_t payload_len, uint32_t now_ms);
    static bool proto_icmp_handler(void* ctx, const EthHdr* eth, const Ipv4Hdr* ip, const uint8_t* payload, uint32_t payload_len, uint32_t now_ms);

    uint32_t next_hop_ip(uint32_t dst_ip_be) const;

    struct PingOp {
        uint32_t key;
        uint32_t tag;
        uint32_t client_token;
        uint32_t dst_ip_be;
        uint32_t next_hop_ip_be;

        uint16_t ident_be;
        uint16_t seq_be;

        uint32_t deadline_ms;

        uint32_t sent_time_ms;
        uint32_t next_arp_tx_ms;

        Mac dst_mac;

        uint8_t state;
    };

    static uint32_t op_next_wakeup_ms(const PingOp& op);
    static uint32_t recompute_next_wakeup_ms(const Vector<PingOp>& ops, uint32_t now_ms);

    void complete_op(uint32_t op_index, uint32_t now_ms, uint8_t ok);
    static uint32_t make_key(uint16_t ident_be, uint16_t seq_be);

    NetDev& m_dev;
    Arp& m_arp;
    IpConfig m_cfg;

    IpProtoDispatch m_proto_dispatch;

    Vector<PingOp> m_ops;
    U32Map m_key_to_index;
    Vector<PingResult> m_results;

    uint32_t m_next_wakeup_ms;
};

}

#endif
