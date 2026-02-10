// SPDX-License-Identifier: GPL-2.0

#ifndef YOS_NET_IPC_H
#define YOS_NET_IPC_H

#include <stdint.h>

#define NET_IPC_MAGIC 0x4E455432u
#define NET_IPC_VERSION 1u
#define NET_IPC_MAX_PAYLOAD 512u

#define NET_IPC_MSG_HELLO 1u
#define NET_IPC_MSG_STATUS_REQ 2u
#define NET_IPC_MSG_STATUS_RESP 3u
#define NET_IPC_MSG_LINK_LIST_REQ 4u
#define NET_IPC_MSG_LINK_LIST_RESP 5u
#define NET_IPC_MSG_PING_REQ 6u
#define NET_IPC_MSG_PING_RESP 7u
#define NET_IPC_MSG_DNS_REQ 8u
#define NET_IPC_MSG_DNS_RESP 9u

#define NET_IPC_MSG_CFG_GET_REQ 10u
#define NET_IPC_MSG_CFG_GET_RESP 11u
#define NET_IPC_MSG_CFG_SET_REQ 12u
#define NET_IPC_MSG_CFG_SET_RESP 13u
#define NET_IPC_MSG_IFACE_UP_REQ 14u
#define NET_IPC_MSG_IFACE_UP_RESP 15u
#define NET_IPC_MSG_IFACE_DOWN_REQ 16u
#define NET_IPC_MSG_IFACE_DOWN_RESP 17u

#define NET_STATUS_OK 0u
#define NET_STATUS_UNSUPPORTED 1u
#define NET_STATUS_UNREACHABLE 2u
#define NET_STATUS_TIMEOUT 3u
#define NET_STATUS_ERROR 4u

#define NET_LINK_FLAG_PRESENT 1u
#define NET_LINK_FLAG_UP 2u
#define NET_LINK_FLAG_LOOPBACK 4u

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t type;
    uint32_t len;
    uint32_t seq;
} net_ipc_hdr_t;

typedef struct __attribute__((packed)) {
    uint32_t status;
    uint32_t link_count;
    uint32_t flags;
} net_status_resp_t;

typedef struct __attribute__((packed)) {
    char name[16];
    uint8_t mac[6];
    uint8_t pad[2];
    uint32_t flags;
    uint32_t ipv4_addr;
    uint32_t ipv4_mask;
} net_link_info_t;

typedef struct __attribute__((packed)) {
    uint32_t count;
} net_link_list_hdr_t;

typedef struct __attribute__((packed)) {
    uint32_t addr;
    uint32_t timeout_ms;
    uint32_t seq;
} net_ping_req_t;

typedef struct __attribute__((packed)) {
    uint32_t addr;
    uint32_t seq;
    uint32_t status;
    uint32_t rtt_ms;
} net_ping_resp_t;

typedef struct __attribute__((packed)) {
    uint32_t timeout_ms;
    char name[256];
} net_dns_req_t;

typedef struct __attribute__((packed)) {
    uint32_t status;
    uint32_t addr;
} net_dns_resp_t;

#define NET_CFG_F_IP   0x00000001u
#define NET_CFG_F_MASK 0x00000002u
#define NET_CFG_F_GW   0x00000004u
#define NET_CFG_F_DNS  0x00000008u

typedef struct __attribute__((packed)) {
    uint32_t flags;
    uint32_t ip;
    uint32_t mask;
    uint32_t gw;
    uint32_t dns;
} net_cfg_set_t;

typedef struct __attribute__((packed)) {
    uint32_t status;
    uint32_t ip;
    uint32_t mask;
    uint32_t gw;
    uint32_t dns;
} net_cfg_resp_t;

typedef struct {
    uint8_t buf[2048];
    uint32_t r;
    uint32_t w;
} net_ipc_rx_t;

uint32_t net_ipc_rx_count(const net_ipc_rx_t* rx);
void net_ipc_rx_reset(net_ipc_rx_t* rx);
void net_ipc_rx_push(net_ipc_rx_t* rx, const uint8_t* src, uint32_t n);
void net_ipc_rx_peek(const net_ipc_rx_t* rx, uint32_t off, void* dst, uint32_t n);
void net_ipc_rx_drop(net_ipc_rx_t* rx, uint32_t n);

int net_ipc_send(int fd, uint16_t type, uint32_t seq, const void* payload, uint32_t len);
int net_ipc_try_recv(net_ipc_rx_t* rx, int fd, net_ipc_hdr_t* out_hdr, uint8_t* out_payload, uint32_t cap);

#endif
