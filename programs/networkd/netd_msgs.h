#ifndef YOS_NETD_MSGS_H
#define YOS_NETD_MSGS_H

#include <stdint.h>

namespace netd {

struct PingSubmitMsg {
    uint32_t dst_ip_be;
    uint16_t ident_be;
    uint16_t seq_be;
    uint32_t timeout_ms;

    uint32_t tag;
    uint32_t client_token;
};

struct PingResultMsg {
    uint32_t dst_ip_be;
    uint16_t ident_be;
    uint16_t seq_be;
    uint32_t rtt_ms;
    uint8_t ok;

    uint32_t tag;
    uint32_t client_token;
};

struct DnsResolveSubmitMsg {
    uint8_t name_len;
    char name[127];

    uint32_t timeout_ms;

    uint32_t tag;
    uint32_t client_token;
};

struct DnsResolveResultMsg {
    uint32_t ip_be;
    uint8_t ok;

    uint32_t tag;
    uint32_t client_token;
};

enum class CoreReqType : uint8_t {
    PingSubmit = 1,
    DnsResolveSubmit = 2,
};

enum class CoreEvtType : uint8_t {
    PingResult = 1,
    DnsResolveResult = 2,
};

struct CoreReqMsg {
    CoreReqType type;

    union {
        PingSubmitMsg ping;
        DnsResolveSubmitMsg dns;
    };
};

struct CoreEvtMsg {
    CoreEvtType type;

    union {
        PingResultMsg ping;
        DnsResolveResultMsg dns;
    };
};

}

#endif
