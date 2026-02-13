#ifndef YOS_NETD_IPC_H
#define YOS_NETD_IPC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NETD_IPC_MAGIC   0x4449544Eu /* 'NTID' */
#define NETD_IPC_VERSION 1u

#define NETD_IPC_MAX_PAYLOAD 256u

typedef enum {
    NETD_IPC_MSG_PING_REQ = 1,
    NETD_IPC_MSG_PING_RSP = 2,
    NETD_IPC_MSG_ERROR    = 3,
} netd_ipc_msg_type_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t type;
    uint32_t len;
    uint32_t seq;
} netd_ipc_hdr_t;

typedef struct __attribute__((packed)) {
    uint32_t dst_ip_be;
    uint16_t ident_be;
    uint16_t seq_be;
    uint32_t timeout_ms;
} netd_ipc_ping_req_t;

typedef struct __attribute__((packed)) {
    uint32_t dst_ip_be;
    uint16_t ident_be;
    uint16_t seq_be;
    uint32_t rtt_ms;
    uint32_t ok;
} netd_ipc_ping_rsp_t;

typedef struct __attribute__((packed)) {
    int32_t code;
} netd_ipc_error_t;

#ifdef __cplusplus
}
#endif

#endif
