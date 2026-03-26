// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Yula1234

#ifndef HAL_PIC_H
#define HAL_PIC_H

#include <stdint.h>

void pic_init(void);
int pic_unmask_irq(uint8_t irq_line);
void pic_configure_legacy(void);

#endif
