// SPDX-License-Identifier: GPL-2.0

#ifndef YOS_NETWORKD_CONFIG_H
#define YOS_NETWORKD_CONFIG_H

#include <stdint.h>

#define NETD_MAX_CLIENTS 8

#define NETD_ARP_CACHE_SIZE 16

#define NETD_FRAME_MAX 1600u

#define NETD_ICMP_DATA_SIZE 56u

#define NETD_ARP_TIMEOUT_MS 800u

#define NETD_PING_ID 0x1234u

#define NETD_DEFAULT_IP   0x0A00020Fu
#define NETD_DEFAULT_MASK 0xFFFFFF00u
#define NETD_DEFAULT_GW   0x0A000202u
#define NETD_DEFAULT_DNS  0x0A000203u

#endif

