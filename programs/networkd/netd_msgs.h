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

}

#endif
