// SPDX-License-Identifier: GPL-2.0

#ifndef DRIVERS_NE2K_H
#define DRIVERS_NE2K_H

#include <stdint.h>

void ne2k_init(void);
int ne2k_is_initialized(void);
int ne2k_get_mac(uint8_t out_mac[6]);

#endif
