// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#ifndef HAL_DELAY_H
#define HAL_DELAY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern uint64_t g_cpu_tsc_hz;

void hal_calibrate_tsc_hz(void);

void udelay(uint32_t us);
void mdelay(uint32_t ms);

#ifdef __cplusplus
}
#endif

#endif