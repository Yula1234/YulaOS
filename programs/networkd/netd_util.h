// SPDX-License-Identifier: GPL-2.0

#ifndef YOS_NETWORKD_UTIL_H
#define YOS_NETWORKD_UTIL_H

#include <stdint.h>

void netd_set_name(char dst[16], const char* src);

uint16_t netd_htons(uint16_t v);
uint16_t netd_ntohs(uint16_t v);
uint32_t netd_htonl(uint32_t v);
uint32_t netd_ntohl(uint32_t v);

uint16_t netd_checksum16(const void* data, uint32_t len);

int netd_ip_same_subnet(uint32_t a, uint32_t b, uint32_t mask);

#endif

