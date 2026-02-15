#ifndef YOS_NETD_DNS_CLIENT_H
#define YOS_NETD_DNS_CLIENT_H

#include "net_proto.h"
#include "net_vec.h"
#include "net_hash_map.h"
#include "dns_types.h"
#include "udp.h"

#include <stdint.h>

namespace netd {

class Arena;
class Ipv4;
class Udp;
class Arp;

class DnsClient {
public:
    DnsClient(Arena& arena, Ipv4& ipv4, Udp& udp, Arp& arp);

    void set_config(const DnsConfig& cfg);

    bool submit_resolve(const ResolveRequest& req, uint32_t now_ms);

    void step(uint32_t now_ms);

    static bool udp_port_handler(
        void* ctx,
        const Ipv4Hdr* ip,
        uint16_t src_port,
        uint16_t dst_port,
        const uint8_t* payload,
        uint32_t payload_len,
        uint32_t now_ms
    );

    bool poll_result(ResolveResult& out);

    bool try_get_next_wakeup_ms(uint32_t now_ms, uint32_t& out_ms) const;

private:
    enum class OpState : uint8_t {
        ArpWait = 0u,
        Tx = 1u
    };

    struct Op {
        uint64_t key;
        uint32_t tag;
        uint32_t client_token;

        uint32_t dst_ip_be;
        uint32_t next_hop_ip_be;

        uint32_t deadline_ms;
        uint32_t next_arp_tx_ms;

        uint32_t next_tx_ms;
        uint8_t tries;

        uint16_t txid;
        uint16_t src_port;

        Mac dst_mac;

        uint8_t name_len;
        char name[127];

        OpState state;
    };

    static uint32_t op_next_wakeup_ms(const Op& op);
    static uint32_t recompute_next_wakeup_ms(const Vector<Op>& ops, uint32_t now_ms);

    bool try_send_query(Op& op, uint32_t now_ms);
    void complete_op(uint32_t op_index, uint32_t ip_be, uint8_t ok, uint32_t now_ms);

    static uint64_t make_key(uint32_t client_token, uint32_t tag);
    static uint64_t make_resp_key(uint32_t src_ip_be, uint16_t src_port, uint16_t txid);
    static uint64_t make_resp_key(const Op& op);

    static uint16_t alloc_src_port(uint32_t now_ms);

    Ipv4& m_ipv4;
    Udp& m_udp;
    Arp& m_arp;

    DnsConfig m_cfg;

    Vector<Op> m_ops;
    HashMap<uint64_t, uint32_t> m_key_to_index;
    HashMap<uint64_t, uint32_t> m_resp_key_to_index;
    Vector<ResolveResult> m_results;

    uint16_t m_next_txid;

    uint32_t m_next_wakeup_ms;
};

}

#endif
