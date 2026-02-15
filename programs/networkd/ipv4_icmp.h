#ifndef YOS_NETD_IPV4_ICMP_H
#define YOS_NETD_IPV4_ICMP_H

#include "net_proto.h"
#include "arp.h"
#include "net_vec.h"
#include "net_u32_map.h"
#include "ipv4.h"

#include <stdint.h>

namespace netd {

class Ipv4Icmp {
public:
    Ipv4Icmp(Arena& arena, Ipv4& ipv4, Arp& arp);

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

    static bool proto_icmp_handler(
        void* ctx,
        const EthHdr* eth,
        const Ipv4Hdr* ip,
        const uint8_t* payload,
        uint32_t payload_len,
        uint32_t now_ms
    );

private:
    bool handle_icmp(const EthHdr* eth, const Ipv4Hdr* ip, const uint8_t* payload, uint32_t payload_len);

    bool handle_proto_icmp(const EthHdr* eth, const Ipv4Hdr* ip, const uint8_t* payload, uint32_t payload_len, uint32_t now_ms);

    enum class PingState : uint8_t {
        ArpWait = 0u,
        Sent = 1u
    };

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

        PingState state;
    };

    static uint32_t op_next_wakeup_ms(const PingOp& op);
    static uint32_t recompute_next_wakeup_ms(const Vector<PingOp>& ops, uint32_t now_ms);

    void complete_op(uint32_t op_index, uint32_t now_ms, uint8_t ok);
    static uint32_t make_key(uint16_t ident_be, uint16_t seq_be);

    Ipv4& m_ipv4;
    Arp& m_arp;

    Vector<PingOp> m_ops;
    U32Map m_key_to_index;
    Vector<PingResult> m_results;

    uint32_t m_next_wakeup_ms;
};

}

#endif
