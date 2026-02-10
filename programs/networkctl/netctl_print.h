// SPDX-License-Identifier: GPL-2.0

#ifndef YOS_NETWORKCTL_PRINT_H
#define YOS_NETWORKCTL_PRINT_H

#include <stdint.h>

#include <net_ipc.h>

void netctl_print_links(const uint8_t* payload, uint32_t len);
void netctl_print_cfg(const net_cfg_resp_t* cfg);
void netctl_print_usage(void);

#endif
