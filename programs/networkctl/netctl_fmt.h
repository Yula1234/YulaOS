// SPDX-License-Identifier: GPL-2.0

#ifndef YOS_NETWORKCTL_FMT_H
#define YOS_NETWORKCTL_FMT_H

#include <stdint.h>

void netctl_ip4_to_str(uint32_t addr, char* out, uint32_t cap);
void netctl_mac_to_str(const uint8_t mac[6], char* out, uint32_t cap);

#endif
