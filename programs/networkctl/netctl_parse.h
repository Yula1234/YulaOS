// SPDX-License-Identifier: GPL-2.0

#ifndef YOS_NETWORKCTL_PARSE_H
#define YOS_NETWORKCTL_PARSE_H

#include <stdint.h>

int netctl_parse_u32(const char* s, uint32_t* out);
int netctl_parse_ip4(const char* s, uint32_t* out);

#endif
